#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>

#define MAX_NAMELEN 1024

#if defined(__TEST__)
#define pfree free
#define palloc malloc
#define IGNORE_POSTGRES_INCLUDES 1
#define fail(format, ...)                       \
    do {                                        \
        fprintf(stderr, format, ##__VA_ARGS__); \
        exit(-1);                               \
    } while (0);
#else
#define fail(format, ...)                                               \
    ereport(ERROR,                                                      \
            (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),              \
             errmsg("invalid input syntax for prometheus sample: " format, \
                    ##__VA_ARGS__)));
#include <postgres.h>
#include "utils.h"
#endif

#include "prom.h"
#include "parse.h"

typedef struct PrometheusParseCtx
{
    PrometheusSample *sample;
    uint16 numlabels;
    uint32 numchars;
} PrometheusParseCtx;

static int
parse_labels(char *input, PrometheusParseCtx *ctx)
{
    PrometheusLabel *curr = ctx->sample == NULL ? NULL : PROM_LABELS(ctx->sample);
    int i = 0;

    if (input[i] != '{')
    {
        return 0;
    }

    ctx->numlabels = 0;
    ctx->numchars = 0;

    /* Skip opening brace */
    i++;

    while (input[i] != '}' && input[i] != '\0')
    {
        int count;
        char labelname[MAX_NAMELEN];
        int value_idx;
        char *value_start;
        size_t namelen;
        size_t valuelen;

        count = sscanf(input + i, "%c%1023[a-zA-Z0-9_]=%n",
                       &labelname[0], &labelname[1], &value_idx);

        if (count != 2)
        {
            fail("Unexpected number of input items assigned: %d\n", count);
        }

        if (!((labelname[0] >= 'a' &&
               labelname[0] <= 'z') ||
              (labelname[0] >= 'A' &&
               labelname[0] <= 'Z') ||
              labelname[0] == '_'))
        {
            /* Invalid first char */
            fail("Invalid first label name char %c\n", labelname[0]);
        }

        namelen = value_idx - 1;
        i += value_idx;

        if (input[i] != '"')
        {
            fail("Label not enclosed by double quotes (start)\n");
        }

        i++;

        value_start = input + i;

        while (input[i] != '"' && input[i] != '}')
        {
            i++;
        }

        if (input[i] != '"')
        {
            fail("Label not enclosed by double quotes (end): %c\n", input[i]);
        }

        valuelen = input + i - value_start;

        if (curr != NULL)
        {
            PROM_LABEL_NAME_SET(curr, labelname, namelen);
            PROM_LABEL_VALUE_SET(curr, value_start, valuelen);
            curr = PROM_LABEL_NEXT(curr);
        }

        i++;
        ctx->numlabels++;
        ctx->numchars += namelen + valuelen + 2;

        if (input[i] == ',')
        {
            i++;
        } else {
            break;
        }
    }

    if (input[i] != '}')
    {
        /* Unexpected state */
        fail("Unexpected end char %c\n", input[i]);
    }

    return i + 1;
}

PrometheusSample *
prom_from_cstring(char *input)
{
    int ret;
    char       metric_name[MAX_NAMELEN];
    double     value;
    int64_t    time_ms;
    int        label_start_idx;
    //size_t label_count;
    size_t metric_namelen;
    PrometheusParseCtx ctx = { 0 };
    PrometheusSample *sample;
    size_t samplelen;

    ret = sscanf(input, "%c%1023[a-zA-Z0-9_:]%n",
                 &metric_name[0], &metric_name[1], &label_start_idx);

    if (ret != 2)
    {
        fail("Unexpected number of input items assigned: %d\n", ret);
    }

    /* Check first char */
    if (!((metric_name[0] >= 'a' &&
           metric_name[0] <= 'z') ||
          (metric_name[0] >= 'A' &&
           metric_name[0] <= 'Z') ||
          metric_name[0] == '_'  ||
          metric_name[0] == ':'))
    {
        /* Invalid first char */
        fail("Invalid first char %c\n", metric_name[0]);
    }

    input += label_start_idx;
    metric_namelen = label_start_idx;

    /* Calculate labels length */
    ret = parse_labels(input, &ctx);

    if (ret < 0)
    {
        fail("Bad label set\n");
    }
    samplelen = PROM_ALLOC_LEN(metric_namelen, ctx.numlabels, ctx.numchars);

    sample = palloc(samplelen);
    memset(sample, 0, samplelen);
    sample->numlabels = ctx.numlabels;
    SET_VARSIZE(sample, samplelen);
    PROM_NAME_SET(sample, metric_name, metric_namelen);
    ctx.sample = sample;

    if (ctx.numlabels > 0)
    {
        /* Parse for real */
        ret = parse_labels(input, &ctx);

        if (ret < 0)
        {
            fail("Bad label set\n");
        }
    }

    input += ret;

    ret = sscanf(input, " %lf %" PRId64 "", &value, &time_ms);

    if (ret < 1)
    {
        fail("Unexpected number of input items assigned\n");
    }

    if (ret == 1)
    {
        struct timeval now;
        gettimeofday(&now, NULL);
        time_ms = now.tv_sec * 1000 + now.tv_usec / 1000;
    }

    sample->value = value;
#if defined(__TEST__)
    sample->time = time_ms;
#else
    sample->time = pg_unix_microseconds_to_timestamp(time_ms * 1000);
#endif

#if defined(__TEST__)
    {
        PrometheusLabel *label;
        int i;

        printf("Varsize is %u\n", VARSIZE(sample));
        printf("Label datalen is %zu\n", PROM_LABEL_DATALEN(sample));

        for (i = 0; i < VARSIZE(sample); i++)
        {
            char c = ((char*)sample)[i];

            if (i % 16 == 0)
                printf("\n");

            if (i % 8 == 0)
                printf("  ");

            if ((c >= 'a' && c <= 'z') || c == '_')
            {
                printf("%2c ", c);
            } else {
                printf("%02x ", c & 0xff);
            }
        }
        printf("\n");

        label = PROM_LABELS(sample);

        printf("name: %s len=%zu\n", PROM_NAME(sample), PROM_NAME_LEN(sample));

        for (i = 0; i < sample->numlabels; i++)
        {
            printf("%s='%s'\n", PROM_LABEL_NAME(label), PROM_LABEL_VALUE(label));
            printf("%zu='%zu'\n", PROM_LABEL_NAME_LEN(label), PROM_LABEL_VALUE_LEN(label));
            label = PROM_LABEL_NEXT(label);
        }

        printf("value: %lf\n", sample->value);
        printf("time: %" PRId64 "\n", sample->time);
    }

    return sample;
#endif

    return sample;
}

#if defined(__TEST__)
int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Unexpected number of arguments: %d\n", argc);
        return -1;
    }

    prom_from_cstring(argv[1]);

    return 0;
}
#endif

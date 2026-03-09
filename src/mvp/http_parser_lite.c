#include "http_parser_lite.h"

#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t * find_seq(const uint8_t * data, size_t len, const char * seq)
{
    size_t i;
    size_t n = strlen(seq);

    if ((NULL == data) || (0U == len) || (NULL == seq) || (0U == n) || (len < n))
    {
        return NULL;
    }

    for (i = 0U; i <= (len - n); i++)
    {
        if (0 == memcmp(data + i, seq, n))
        {
            return data + i;
        }
    }

    return NULL;
}

static const uint8_t * find_http_start(const uint8_t * data, size_t len)
{
    size_t i;

    if ((NULL == data) || (len < 5U))
    {
        return NULL;
    }

    for (i = 0U; i + 5U <= len; i++)
    {
        if (0 == memcmp(data + i, "HTTP/", 5U))
        {
            return data + i;
        }
    }

    return NULL;
}

static int ascii_case_eq(char a, char b)
{
    return (tolower((unsigned char) a) == tolower((unsigned char) b));
}

static const char * find_header_ci(const char * headers, const char * key)
{
    size_t i;
    size_t key_len;
    size_t len;

    if ((NULL == headers) || (NULL == key))
    {
        return NULL;
    }

    len = strlen(headers);
    key_len = strlen(key);

    if (len < key_len)
    {
        return NULL;
    }

    for (i = 0U; i <= len - key_len; i++)
    {
        size_t j;
        int match = 1;
        for (j = 0U; j < key_len; j++)
        {
            if (!ascii_case_eq(headers[i + j], key[j]))
            {
                match = 0;
                break;
            }
        }

        if (match)
        {
            return headers + i;
        }
    }

    return NULL;
}

int http_lite_parse(const uint8_t * data, size_t size, http_lite_response_t * out)
{
    const uint8_t * start;
    const uint8_t * h_end;
    char header_copy[1024];
    const char * cl;
    int status = 0;
    size_t header_len;
    size_t available;

    if ((NULL == data) || (0U == size) || (NULL == out))
    {
        return MVP_ERR_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->status_code = -1;
    out->content_length = -1;

    start = find_http_start(data, size);
    if (NULL == start)
    {
        return MVP_ERR_HTTP;
    }

    available = size - (size_t) (start - data);

    h_end = find_seq(start, available, "\r\n\r\n");
    if (NULL == h_end)
    {
        h_end = find_seq(start, available, "\n\n");
        if (NULL == h_end)
        {
            return MVP_ERR_HTTP;
        }
        header_len = (size_t) (h_end - start) + 2U;
    }
    else
    {
        header_len = (size_t) (h_end - start) + 4U;
    }

    out->header_len = header_len;

    if ((header_len + 1U) >= sizeof(header_copy))
    {
        return MVP_ERR_OVERFLOW;
    }

    memcpy(header_copy, start, header_len);
    header_copy[header_len] = '\0';

    if (1 != sscanf(header_copy, "HTTP/%*u.%*u %d", &status))
    {
        return MVP_ERR_HTTP;
    }
    out->status_code = status;

    cl = find_header_ci(header_copy, "Content-Length:");
    if (NULL != cl)
    {
        out->content_length = atoi(cl + strlen("Content-Length:"));
    }

    out->body = start + header_len;
    out->body_len = available - header_len;

    if (out->content_length >= 0)
    {
        out->complete = (out->body_len >= (size_t) out->content_length);
        if (out->complete)
        {
            out->body_len = (size_t) out->content_length;
        }
    }
    else
    {
        out->complete = (out->body_len > 0U);
    }

    return MVP_OK;
}

static const char * find_json_key(const char * json, const char * key)
{
    char pattern[64];
    (void) snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static int parse_json_int(const char * json, const char * key, int * out_value)
{
    const char * p = find_json_key(json, key);
    if ((NULL == p) || (NULL == out_value))
    {
        return MVP_ERR_JSON;
    }

    p = strchr(p, ':');
    if (NULL == p)
    {
        return MVP_ERR_JSON;
    }

    p++;
    while (isspace((unsigned char) *p))
    {
        p++;
    }

    *out_value = (int) strtol(p, NULL, 10);
    return MVP_OK;
}

/* MicroLib strtod returns 0.  Manual decimal parser. */
static float mvp_strtof_local(const char * s)
{
    int sign = 1;
    int64_t integer_part = 0;
    int64_t frac_part = 0;
    int64_t frac_div = 1;
    int has_digit = 0;

    if (NULL == s) { return 0.0f; }
    while ((*s == ' ') || (*s == '\t')) { s++; }
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while ((*s >= '0') && (*s <= '9'))
    {
        has_digit = 1;
        integer_part = integer_part * 10 + (*s - '0');
        s++;
    }

    if (*s == '.')
    {
        s++;
        while ((*s >= '0') && (*s <= '9'))
        {
            has_digit = 1;
            frac_part = frac_part * 10 + (*s - '0');
            frac_div *= 10;
            s++;
        }
    }

    if (!has_digit) { return 0.0f; }
    return (float) sign * ((float) integer_part + (float) frac_part / (float) frac_div);
}

static int parse_json_float(const char * json, const char * key, float * out_value)
{
    const char * p = find_json_key(json, key);
    if ((NULL == p) || (NULL == out_value))
    {
        return MVP_ERR_JSON;
    }

    p = strchr(p, ':');
    if (NULL == p)
    {
        return MVP_ERR_JSON;
    }

    p++;
    while (isspace((unsigned char) *p))
    {
        p++;
    }

    *out_value = mvp_strtof_local(p);
    return MVP_OK;
}

static int parse_json_text(const char * json, const char * key, char * out_text, size_t text_size)
{
    const char * p;
    const char * q;
    size_t len;

    if ((NULL == json) || (NULL == key) || (NULL == out_text) || (text_size < 2U))
    {
        return MVP_ERR_ARG;
    }

    p = find_json_key(json, key);
    if (NULL == p)
    {
        return MVP_ERR_JSON;
    }

    p = strchr(p, ':');
    if (NULL == p)
    {
        return MVP_ERR_JSON;
    }

    p = strchr(p, '"');
    if (NULL == p)
    {
        return MVP_ERR_JSON;
    }
    p++;

    q = p;
    while ((*q != '\0') && (*q != '"'))
    {
        q++;
    }

    if ('"' != *q)
    {
        return MVP_ERR_JSON;
    }

    len = (size_t) (q - p);
    if (len >= text_size)
    {
        len = text_size - 1U;
    }

    memcpy(out_text, p, len);
    out_text[len] = '\0';
    return MVP_OK;
}

int http_lite_extract_json_result(const uint8_t * body,
                                  size_t body_len,
                                  int * out_code,
                                  char * out_text,
                                  size_t text_size,
                                  float * out_confidence)
{
    char json[2048];
    int rc;

    if ((NULL == body) || (0U == body_len) || (NULL == out_code) ||
        (NULL == out_text) || (NULL == out_confidence))
    {
        return MVP_ERR_ARG;
    }

    if (body_len >= sizeof(json))
    {
        return MVP_ERR_OVERFLOW;
    }

    memcpy(json, body, body_len);
    json[body_len] = '\0';

    rc = parse_json_int(json, "code", out_code);
    if (MVP_OK != rc)
    {
        return rc;
    }

    rc = parse_json_text(json, "text", out_text, text_size);
    if (MVP_OK != rc)
    {
        out_text[0] = '\0';
    }

    rc = parse_json_float(json, "confidence", out_confidence);
    if (MVP_OK != rc)
    {
        *out_confidence = 0.0f;
    }

    return MVP_OK;
}

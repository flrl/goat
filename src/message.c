#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goat.h"

#include "irc.h"
#include "message.h"

#define _has_crlf(s)    (strcspn((s), "\r\n") < strlen((s)))
#define _has_sp(s)      (NULL != strchr((s), ' '))

static const char *_next_tag(const char *str);
static const char *_find_tag(const char *str, const char *key);
static const char *_find_value(const char *str);
static const char *_escape_value(const char *value, char *buf, size_t *size);
static const char *_unescape_value(const char *value, char *buf, size_t *size);

goat_message_t *goat_message_new(const char *prefix, const char *command, const char **params) {
    assert(command != NULL);
    size_t len = 0, n_params = 0;

    if (prefix != NULL) {
        if (_has_crlf(prefix)) return NULL;
        if (_has_sp(prefix)) return NULL;
        len += strlen(prefix) + 2;
    }

    if (_has_crlf(command)) return NULL;
    if (_has_sp(command)) return NULL;
    len += strlen(command);

    if (params) {
        int have_space_param = 0;

        for (const char **p = params; *p; p++) {
            // a param may not start with :
            if (**p == ':') return NULL;

            if (_has_crlf(*p)) return NULL;

            // further parameters after one containing a space are invalid
            if (have_space_param) return NULL;
            if (_has_sp(*p)) have_space_param = 1;

            len += strlen(*p) + 1;
            ++ n_params;
            if (n_params == 15)  break;
        }
        len += 1;
    }
    if (len > 510)  return NULL;

    goat_message_t *message = calloc(1, sizeof(goat_message_t));
    if (message == NULL)  return NULL;

    char *position = message->m_bytes;

    if (prefix) {
        *position++ = ':';
        message->m_prefix = position;
        position = stpcpy(position, prefix);
        ++ position;
    }

    if (0 == goat_command(command, &message->m_command)) {
        message->m_have_recognised_command = 1;
        message->m_command_string = goat_command_string(message->m_command);
    }
    else {
        message->m_command_string = position;
    }
    position = stpcpy(position, command);

    if (params) {
        size_t i;
        for (i = 0; i < n_params - 1; i++) {
            ++position;
            message->m_params[i] = position;
            position = stpcpy(position, params[i]);
        }
        ++position;
        *position++ = ':';
        message->m_params[i] = position;
        position = stpcpy(position, params[i]);
    }

    message->m_len = position - message->m_bytes;
    return message;
}

goat_message_t *goat_message_new_from_string(const char *str, size_t len) {
    assert(str != NULL);
    assert(len > 0);
    assert(len == strnlen(str, len + 5));

    // chomp crlf
    if (str[len - 1] == '\x0a') {
        -- len;
        if (str[len - 1] == '\x0d')  -- len;
    }

    goat_message_t *message = calloc(1, sizeof(goat_message_t));
    if (message == NULL)  return NULL;

    message->m_len = len;
    strncpy(message->m_bytes, str, len);

    if (_has_crlf(message->m_bytes)) goto cleanup;

    char *position = message->m_bytes;
    char *token;

    // FIXME tags

    // [ ':' prefix SPACE ]
    if (position[0] == ':') {
        ++ position;
        token = strsep(&position, " ");
        if (token[0] == '\0')  goto cleanup;
        message->m_prefix = token;
    }

    // command
    token = strsep(&position, " ");
    if (token == NULL || token[0] == '\0')  goto cleanup;
    if (0 == goat_command(token, &message->m_command)) {
        message->m_have_recognised_command = 1;
        message->m_command_string = goat_command_string(message->m_command);
    }
    else {
        message->m_command_string = token;
    }

    // *14( SPACE middle ) [ SPACE ":" trailing ]
    // 14( SPACE middle ) [ SPACE [ ":" ] trailing ]
    unsigned i = 0;
    while (i < 14 && position) {
        if (position[0] == ':')  break;
        token = strsep(&position, " ");
        message->m_params[i] = token;
        ++ i;
    }
    if (position && position[0] == ':')  ++position;
    message->m_params[i] = position;

    return message;

cleanup:
    free(message);
    return NULL;
}

goat_message_t *goat_message_clone(const goat_message_t *orig) {
    assert(orig != NULL);

    goat_message_t *clone = calloc(1, sizeof(goat_message_t));
    if (NULL == clone) return NULL;

    // FIXME tags

    memcpy(clone->m_bytes, orig->m_bytes, orig->m_len);
    clone->m_len = orig->m_len;

    if (orig->m_prefix) {
        clone->m_prefix = clone->m_bytes + (orig->m_prefix - orig->m_bytes);
    }

    if (orig->m_have_recognised_command) {
        clone->m_have_recognised_command = orig->m_have_recognised_command;
        clone->m_command = orig->m_command;
        clone->m_command_string = goat_command_string(clone->m_command);
        assert(clone->m_command_string == orig->m_command_string);
    }
    else {
        clone->m_command_string = clone->m_bytes + (orig->m_command_string - orig->m_bytes);
    }

    for (int i = 0; i < 16; i++) {
        if (orig->m_params[i]) {
            clone->m_params[i] = clone->m_bytes + (orig->m_params[i] - orig->m_bytes);
        }
    }

    return clone;
}

void goat_message_delete(goat_message_t *message) {
    if (message->m_tags) free(message->m_tags);
    free(message);
}

char *goat_message_strdup(const goat_message_t *message) {
    if (NULL == message) return NULL;

    size_t len = message->m_len + 1;
    char *str = malloc(len);
    if (str == NULL)  return NULL;

    if (goat_message_cstring(message, str, &len)) {
        return str;
    }

    free(str);
    return NULL;
}

char *goat_message_cstring(const goat_message_t *message, char *buf, size_t *len) {
    if (NULL == message) return NULL;
    if (NULL == buf) return NULL;
    if (NULL == len) return NULL;
    assert(*len > message->m_len);

    // FIXME tags

    if (*len > message->m_len) {
        memcpy(buf, message->m_bytes, message->m_len);
        memset(buf + message->m_len, 0, *len - message->m_len);
        *len = message->m_len;

        for (unsigned i = 0; i < message->m_len; i++) {
            if (buf[i] == '\0') buf[i] = ' ';
        }

        return buf;
    }

    return NULL;
}

const char *goat_message_get_prefix(const goat_message_t *message) {
    if (NULL == message) return NULL;

    return message->m_prefix;
}

const char *goat_message_get_command_string(const goat_message_t *message) {
    if (NULL == message) return NULL;

    return message->m_command_string;
}

const char *goat_message_get_param(const goat_message_t *message, size_t index) {
    if (NULL == message) return NULL;
    if (index >= 16) return NULL;

    return message->m_params[index];
}

size_t goat_message_get_nparams(const goat_message_t *message) {
    if (NULL == message) return 0;

    size_t i;

    for (i = 0; i < 16 && message->m_params[i]; i++)
        ;

    return i;
}

int goat_message_get_command(const goat_message_t *message, goat_command_t *command) {
    if (NULL == message) return -1;
    if (NULL == command) return -1;

    if (message->m_have_recognised_command) {
        *command = message->m_command;
        return 0;
    }

    return -1;
}

size_t goat_message_has_tags(const goat_message_t *message) {
    assert(message != NULL);

    const goat_message_tags_t *tags = message->m_tags;

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return 0;

    size_t count = 0;
    const char *p = tags->m_bytes;
    while (*p != '\0') {
        if (*p == ';') count++;
        p++;
    }

    return count;
}

int goat_message_has_tag(const goat_message_t *message, const char *key) {
    assert(message != NULL);

    const goat_message_tags_t *tags = message->m_tags;

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return 0;

    return _find_tag(tags->m_bytes, key) ? 1 : 0;
}

int goat_message_get_tag_value(
    const goat_message_t *message, const char *key, char *value, size_t *size
) {
    assert(message != NULL);
    assert(key != NULL);
    assert(value != NULL);
    assert(size != NULL);

    // FIXME make sure buffer is big enough...

    const goat_message_tags_t *tags = message->m_tags;
    memset(value, 0, *size);

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return 0;

    const char *p, *v, *end;

    if (NULL == (p = _find_tag(tags->m_bytes, key))) {
        *size = 0;
        return 0;
    }

    if (NULL == (v = _find_value(p))) {
        *size = 0;
        return 0;
    }

    end = _next_tag(v);
    if (*end) end--;
    *size = end - v;

    char unescaped[GOAT_MESSAGE_MAX_TAGS];
    size_t unescaped_len = sizeof(unescaped);
    _unescape_value(v, unescaped, &unescaped_len);
    strncpy(value, unescaped, *size);
    // FIXME sort out *size properly

    return 1;
}

int goat_message_set_tag(goat_message_t *message, const char *key, const char *value) {
    assert(message != NULL);
    assert(key != NULL);

    char escaped_value[GOAT_MESSAGE_MAX_TAGS];
    size_t escaped_value_len = sizeof(escaped_value);

    goat_message_tags_t *tags = message->m_tags;

    size_t tag_len = 1 + strlen(key);
    if (value) {
        _escape_value(value, escaped_value, &escaped_value_len);
        tag_len += 1 + escaped_value_len;
    }

    if (tags->m_len + tag_len > GOAT_MESSAGE_MAX_TAGS) return -1;

    char *p, kvbuf[GOAT_MESSAGE_MAX_TAGS], tmp[GOAT_MESSAGE_MAX_TAGS] = {0};
    const size_t key_len = strlen(key);

    p = tags->m_bytes;
    int cmp;
    while (p && *p) {
        if ((cmp = strncmp(p, key, key_len)) >= 0) break;

        p = (char *) _next_tag(p);
    }

    const char *rest = (0 == cmp ? _next_tag(p) : p);
    strcpy(tmp, rest);

    snprintf(kvbuf, sizeof(kvbuf), "%s=%s;", key, escaped_value);

    p = stpcpy(p, kvbuf);
    strcpy(p, tmp);

    return 0;
}

int goat_message_unset_tag(goat_message_t *message, const char *key) {
    assert(message != NULL);
    assert(key != NULL);

    goat_message_tags_t *tags = message->m_tags;

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return 0;

    char *p1, *p2, *end;

    p1 = (char *) _find_tag(tags->m_bytes, key);
    if (!*p1) return 0;

    end = &tags->m_bytes[tags->m_len];

    p2 = (char *) _next_tag(p1);

    if (*p2) {
        memmove(p1, p2, end - p2);
        tags->m_len -= (p2 - p1);
        memset(&tags->m_bytes[tags->m_len], 0, sizeof(tags->m_bytes) - tags->m_len);
    }
    else {
        tags->m_len = p1 - tags->m_bytes;
        memset(p1, 0, sizeof(tags->m_bytes) - tags->m_len);
    }

    return 1;
}

const char *_next_tag(const char *str) {
    assert(str != NULL);

    const char *p = str;

    while (*p != '\0' && *p != ';') p++;

    if (*p == ';') return p + 1;

    return p;
}

const char *_find_tag(const char *str, const char *key) {
    assert(str != NULL);
    assert(key != NULL);

    const char *p = str;
    const size_t key_len = strlen(key);

    while (*p) {
        if (0 == strncmp(p, key, key_len)) {
            switch (*(p + key_len)) {
                case '\0':
                case '=':
                case ';':
                    return p;
            }
        }

        p = _next_tag(p);
    }

    return NULL;
}

const char *_find_value(const char *str) {
    assert(str != NULL);

    const char *p = str;

    while (*p != '\0' && *p != ';' && *p != '=') p++;

    if (*p == '=' && *(p + 1) != '\0') return p + 1;

    return NULL;
}

const char *_escape_value(const char *value, char *buf, size_t *size) {
    assert(value != NULL);
    assert(buf != NULL);
    assert(size != NULL);

    char *dest = buf;

    for (size_t i = 0; i < *size; i++) {
        switch(value[i]) {
            case ';':
                dest[0] = '\\';
                dest[1] = ':';
                dest += 2;
                break;

            case ' ':
                dest[0] = '\\';
                dest[1] = 's';
                dest += 2;
                break;

            case '\0':
                dest[0] = '\\';
                dest[1] = '0';
                dest += 2;
                break;

            case '\\':
                // FIXME spec says just single '\', is that a typo?
                // https://github.com/ircv3/ircv3-specifications/blob/master/specification/message-tags-3.2.md
                dest[0] = '\\';
                dest[1] = '\\';
                dest += 2;
                break;

            case '\r':
                dest[0] = '\\';
                dest[1] = 'r';
                dest += 2;
                break;

            case '\n':
                dest[0] = '\\';
                dest[1] = 'n';
                dest += 2;
                break;

            default:
                *dest++ = value[i];
                break;
        }
    }

    *dest = '\0';
    *size = dest - buf;

    return buf;
}

const char *_unescape_value(const char *value, char *buf, size_t *size) {
    assert(value != NULL);
    assert(buf != NULL);
    assert(size != NULL);

    char *dest = buf;

    size_t value_len = strlen(value);
    assert(value_len > *size);

    for (size_t i = 0; i < value_len; i++) {
        if (value[i] == '\\') {
            char c = value[i + 1];
            switch (c) {
                case ':':  c = ';';          break;
                case 's':  c = ' ';          break;
                case '0':  c = '\0';         break;
                case '\\': c = '\\';         break;
                case 'r':  c = '\r';         break;
                case 'n':  c = '\n';         break;
            }
            *dest++ = c;
            i++;
        }
        else {
            *dest++ = value[i];
        }
    }
    memset(dest, 0, *size - (dest - buf));

    *size = dest - buf;
    return buf;
}

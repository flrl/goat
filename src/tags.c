#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "goat.h"

#include "message.h"
#include "tags.h"
#include "util.h"

static const char *_next_tag(const char *str);
static const char *_find_tag(const char *str, const char *key);
static const char *_find_value(const char *str);
static const char *_escape_value(const char *value, char *buf, size_t *size);
static const char *_unescape_value(const char *value, char *buf, size_t *size);

GoatError tags_init(MessageTags **tagsp, const char *key, const char *value) {
    char escaped_value[GOAT_MESSAGE_MAX_TAGS];
    size_t escaped_value_len = sizeof(escaped_value);

    size_t len = strlen(key);
    if (value) {
        _escape_value(value, escaped_value, &escaped_value_len);
        len += 1 + escaped_value_len;  // = and value
    }

    MessageTags *tags = calloc(1, sizeof(*tags));
    if (NULL == tags) return errno;

    if (len > GOAT_MESSAGE_MAX_TAGS) return GOAT_E_MSGLEN;

    if (value) {
        snprintf(tags->m_bytes, len + 1, "%s=%s", key, escaped_value);
    }
    else {
        snprintf(tags->m_bytes, len + 1, "%s", key);
    }

    *tagsp = tags;
    return 0;
}

size_t goat_message_has_tags(const GoatMessage *message) {
    if (NULL == message) return 0;

    const MessageTags *tags = message->m_tags;

    if (NULL == tags) return 0;
    if (tags->m_bytes[0] == '\0') return 0;

    size_t count = 1;
    const char *p = tags->m_bytes;
    while (p[0] != '\0') {
        if (p[0] == ';' && p[1] != '\0') count++;
        p++;
    }

    return count;
}

int goat_message_has_tag(const GoatMessage *message, const char *key) {
    if (NULL == message) return 0;
    if (NULL == key) return 0;

    const MessageTags *tags = message->m_tags;

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return 0;

    return _find_tag(tags->m_bytes, key) ? 1 : 0;
}

GoatError goat_message_get_tag_value(
    const GoatMessage *message, const char *key, char *value, size_t *size
) {
    if (NULL == message) return EINVAL;
    if (NULL == key) return EINVAL;
    if (NULL == value) return EINVAL;
    if (NULL == size) return EINVAL;

    const MessageTags *tags = message->m_tags;

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return GOAT_E_NOTAG;

    const char *p, *v, *end;

    p = _find_tag(tags->m_bytes, key);
    if (NULL == p) return GOAT_E_NOTAG;

    v = _find_value(p);
    if (NULL == v) return GOAT_E_NOTAGVAL;

    end = _next_tag(v);
    if ('\0' != *end) end--; // account for separator

    char unescaped[GOAT_MESSAGE_MAX_TAGS];
    size_t unescaped_len = sizeof(unescaped);
    _unescape_value(v, unescaped, &unescaped_len);

    if (*size <= unescaped_len) return EOVERFLOW;

    memset(value, 0, *size);
    strncpy(value, unescaped, unescaped_len);
    *size = unescaped_len;

    return 0;
}

GoatError goat_message_set_tag(GoatMessage *message, const char *key, const char *value) {
    if (NULL == message) return EINVAL;
    if (NULL == key) return EINVAL;

    if (NULL == message->m_tags) {
        return tags_init(&message->m_tags, key, value);
    }

    if (_find_tag(message->m_tags->m_bytes, key)) {
        // tag already exists, discard the old one
        goat_message_unset_tag(message, key);
    }

    char buf[GOAT_MESSAGE_MAX_TAGS] = {0};
    char *p = buf;

    if (message->m_tags->m_len + strlen(key) + 1 > GOAT_MESSAGE_MAX_TAGS) {
        return GOAT_E_MSGLEN;
    }

    if (message->m_tags->m_len > 0) {
        // separator if there's already tag data
        *p++ = ';';
    }

    p = stpcpy(p, key);

    if (value) {
        char escaped_value[GOAT_MESSAGE_MAX_TAGS] = {0};
        size_t escaped_value_len = sizeof(escaped_value);

        _escape_value(value, escaped_value, &escaped_value_len);

        if (message->m_tags->m_len + strlen(buf) + escaped_value_len + 1 > GOAT_MESSAGE_MAX_TAGS) {
            return GOAT_E_MSGLEN;
        }

        *p++ = '=';
        p = stpcpy(p, escaped_value);
    }

    strcat(message->m_tags->m_bytes, buf);
    message->m_tags->m_len += strlen(buf);

    return 0;
}

GoatError goat_message_unset_tag(GoatMessage *message, const char *key) {
    if (NULL == message) return EINVAL;
    if (NULL == key) return EINVAL;

    MessageTags *tags = message->m_tags;

    if (NULL == tags || 0 == strlen(tags->m_bytes)) return 0;

    char *p1, *p2, *end;

    end = &tags->m_bytes[tags->m_len];

    p1 = (char *) _find_tag(tags->m_bytes, key);
    if (p1[0] == '\0') return 0;
    // => p1 is the start of the tag to be replaced

    p2 = (char *) _next_tag(p1);
    // => p2 is the start of the rest of the string (if any)

    if (p2[0] != '\0') {
        memmove(p1, p2, end - p2);
        tags->m_len -= (p2 - p1);
        memset(&tags->m_bytes[tags->m_len], 0, sizeof(tags->m_bytes) - tags->m_len);
    }
    else {
        tags->m_len = p1 - tags->m_bytes;
        memset(p1, 0, sizeof(tags->m_bytes) - tags->m_len);
    }

    // may be a trailing semicolon if tag removed from end, chomp it
    if (tags->m_bytes[tags->m_len - 1] == ';') {
        tags->m_bytes[-- tags->m_len] = '\0';
    }

    return 0;
}

size_t tags_parse(const char *str, MessageTags **tagsp) {
    if (NULL == tagsp) return 0;
    if (str[0] != '@') return 0;

    // populate our struct, skipping the @
    char *end = strchr(&str[1], ' ');
    size_t len = end - &str[1];

    if (strn_has_crlf(&str[1], len)) return 0;
    if (strn_has_sp(&str[1], len)) return 0;

    if (len > 0 && len <= GOAT_MESSAGE_MAX_TAGS) {
        MessageTags *tags = calloc(1, sizeof(MessageTags));
        if (NULL == tags) return 0;

        tags->m_len = len;
        strncpy(tags->m_bytes, &str[1], len);

        *tagsp = tags;
    }

    // calculate consumed length (including @ and space)
    if (*end != '\0') end ++;
    size_t consumed = end - str;

    return consumed;
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

    for (size_t i = 0; i < *size && value[i] != '\0'; i++) {
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

            case '\\':
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
    assert(value_len < *size);

    for (size_t i = 0; i < value_len; i++) {
        if (value[i] == '\\') {
            char c = value[i + 1];
            switch (c) {
                case ':':  c = ';';          break;
                case 's':  c = ' ';          break;
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

/**
 * json.c - just enough JSON for the LUKS2 metadata area.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Recursive descent over a copy of the text; every node and every string
 * lives in one arena sized from the input (a node per character is the
 * worst case), so a document is freed at once. Numbers are kept as written:
 * LUKS2 writes 64-bit sizes as strings anyway, and callers ask for what
 * they need (JsonU64). Nesting is bounded; malformed input fails cleanly.
 */

#include "ext4ctl.h"

#define JSON_MAX_DEPTH  32

typedef struct _JSON_PARSER {
    JSON_DOC   *Doc;
    const char *p;
    const char *End;
    int         Depth;
} JSON_PARSER;

static void *
Take(JSON_DOC *Doc, size_t Size)
{
    void *Block;

    Size = (Size + 7) & ~(size_t)7;
    if (Doc->Used + Size > Doc->Size) {
        return NULL;
    }
    Block = Doc->Arena + Doc->Used;
    Doc->Used += Size;
    memset(Block, 0, Size);
    return Block;
}

static void
SkipSpace(JSON_PARSER *P)
{
    while (P->p < P->End && (*P->p == ' ' || *P->p == '\t' || *P->p == '\r' || *P->p == '\n')) {
        P->p++;
    }
}

static int
HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* a string at P->p (on the opening quote); escapes resolved, \u to UTF-8 */
static const char *
ParseString(JSON_PARSER *P)
{
    const char *Start = ++P->p;
    char       *Out, *o;

    while (P->p < P->End && *P->p != '"') {
        if (*P->p == '\\') {
            P->p++;
        }
        P->p++;
    }
    if (P->p >= P->End) {
        return NULL;
    }
    Out = o = (char *)Take(P->Doc, (size_t)(P->p - Start) + 1);
    if (Out == NULL) {
        return NULL;
    }
    for (const char *s = Start; s < P->p; s++) {
        if (*s != '\\') {
            *o++ = *s;
            continue;
        }
        switch (*++s) {
        case 'b': *o++ = '\b'; break;
        case 'f': *o++ = '\f'; break;
        case 'n': *o++ = '\n'; break;
        case 'r': *o++ = '\r'; break;
        case 't': *o++ = '\t'; break;
        case 'u': {
            unsigned Code = 0;
            int k;
            if (P->p - s < 5) {
                return NULL;
            }
            for (k = 1; k <= 4; k++) {
                int d = HexDigit(s[k]);
                if (d < 0) {
                    return NULL;
                }
                Code = Code * 16 + (unsigned)d;
            }
            s += 4;
            if (Code < 0x80) {
                *o++ = (char)Code;
            } else if (Code < 0x800) {
                *o++ = (char)(0xC0 | (Code >> 6));
                *o++ = (char)(0x80 | (Code & 0x3F));
            } else {
                *o++ = (char)(0xE0 | (Code >> 12));
                *o++ = (char)(0x80 | ((Code >> 6) & 0x3F));
                *o++ = (char)(0x80 | (Code & 0x3F));
            }
            break;
        }
        default:  *o++ = *s; break;     /* \" \\ \/ */
        }
    }
    *o = 0;
    P->p++;                             /* closing quote */
    return Out;
}

static JSON *ParseValue(JSON_PARSER *P);

static JSON *
ParseContainer(JSON_PARSER *P, JSON *Node, char Close, BOOL Members)
{
    JSON **Tail = &Node->Child;

    if (++P->Depth > JSON_MAX_DEPTH) {
        return NULL;
    }
    P->p++;                             /* [ or { */
    SkipSpace(P);
    if (P->p < P->End && *P->p == Close) {
        P->p++;
        P->Depth--;
        return Node;
    }
    for (;;) {
        const char *Key = NULL;
        JSON       *Child;

        SkipSpace(P);
        if (Members) {
            if (P->p >= P->End || *P->p != '"' || (Key = ParseString(P)) == NULL) {
                return NULL;
            }
            SkipSpace(P);
            if (P->p >= P->End || *P->p++ != ':') {
                return NULL;
            }
        }
        Child = ParseValue(P);
        if (Child == NULL) {
            return NULL;
        }
        Child->Key = Key;
        *Tail = Child;
        Tail = &Child->Next;

        SkipSpace(P);
        if (P->p < P->End && *P->p == ',') {
            P->p++;
            continue;
        }
        if (P->p < P->End && *P->p == Close) {
            P->p++;
            P->Depth--;
            return Node;
        }
        return NULL;
    }
}

static JSON *
ParseValue(JSON_PARSER *P)
{
    JSON *Node;

    SkipSpace(P);
    if (P->p >= P->End || (Node = (JSON *)Take(P->Doc, sizeof(JSON))) == NULL) {
        return NULL;
    }

    switch (*P->p) {
    case '{':
        Node->Type = JSON_OBJECT;
        return ParseContainer(P, Node, '}', TRUE);
    case '[':
        Node->Type = JSON_ARRAY;
        return ParseContainer(P, Node, ']', FALSE);
    case '"':
        Node->Type = JSON_STRING;
        Node->Text = ParseString(P);
        return Node->Text ? Node : NULL;
    default: {
        /* number or literal, kept as written */
        const char *Start = P->p;
        char       *Text;
        while (P->p < P->End && strchr(",}] \t\r\n", *P->p) == NULL) {
            P->p++;
        }
        if (P->p == Start || (Text = (char *)Take(P->Doc, (size_t)(P->p - Start) + 1)) == NULL) {
            return NULL;
        }
        memcpy(Text, Start, (size_t)(P->p - Start));
        Node->Text = Text;
        if (strcmp(Text, "null") == 0) {
            Node->Type = JSON_NULL;
        } else if (strcmp(Text, "true") == 0 || strcmp(Text, "false") == 0) {
            Node->Type = JSON_BOOL;
        } else if (*Text == '-' || (*Text >= '0' && *Text <= '9')) {
            Node->Type = JSON_NUMBER;
        } else {
            return NULL;
        }
        return Node;
    }
    }
}

BOOL
JsonParse(JSON_DOC *Doc, const char *Text, size_t Length)
{
    JSON_PARSER P;

    memset(Doc, 0, sizeof(*Doc));
    /* the text ends at the first NUL: the LUKS2 area is zero-padded */
    Length = strnlen(Text, Length);
    Doc->Size = (Length + 1) * (sizeof(JSON) + 8) + 64;
    Doc->Arena = (char *)HeapAlloc(GetProcessHeap(), 0, Doc->Size);
    if (Doc->Arena == NULL) {
        return FALSE;
    }
    P.Doc = Doc;
    P.p = Text;
    P.End = Text + Length;
    P.Depth = 0;
    Doc->Root = ParseValue(&P);
    if (Doc->Root == NULL || Doc->Root->Type != JSON_OBJECT) {
        JsonFree(Doc);
        return FALSE;
    }
    return TRUE;
}

void
JsonFree(JSON_DOC *Doc)
{
    if (Doc->Arena) {
        HeapFree(GetProcessHeap(), 0, Doc->Arena);
    }
    memset(Doc, 0, sizeof(*Doc));
}

const JSON *
JsonGet(const JSON *Object, const char *Key)
{
    const JSON *Child;

    if (Object == NULL || Object->Type != JSON_OBJECT) {
        return NULL;
    }
    for (Child = Object->Child; Child; Child = Child->Next) {
        if (Child->Key && strcmp(Child->Key, Key) == 0) {
            return Child;
        }
    }
    return NULL;
}

const char *
JsonText(const JSON *Node)
{
    return (Node && (Node->Type == JSON_STRING || Node->Type == JSON_NUMBER)) ? Node->Text : NULL;
}

/* a non-negative integer, written as a number or as a string of digits */
BOOL
JsonU64(const JSON *Node, UINT64 *Value)
{
    const char *s = JsonText(Node);
    UINT64      v = 0;

    if (s == NULL || *s == 0) {
        return FALSE;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (MAXUINT64 - 9) / 10) {
            return FALSE;
        }
        v = v * 10 + (UINT64)(*s - '0');
    }
    *Value = v;
    return TRUE;
}

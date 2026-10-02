/**
 * lvm_text.c - the text form of LVM2 metadata: sections, values, lists.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * SPDX-License-Identifier: GPL-2.0-only
 *
 *   name {                      a section
 *       key = value             a word, a number or a "quoted string"
 *       list = ["a", "b", 3]    a list
 *   }                           # comments to the end of the line
 *
 * Parsed into nodes in one arena: nothing is freed but the whole of it.
 */

#include "lvm_internal.h"

static void *
TextTake(LVM_TEXT *T, size_t Size)
{
    void *Block;

    Size = (Size + 7) & ~(size_t)7;
    if (T->Used + Size > T->Size) {
        return NULL;
    }
    Block = T->Arena + T->Used;
    T->Used += Size;
    memset(Block, 0, Size);
    return Block;
}

static void
TextSkip(LVM_TEXT *T)
{
    while (T->p < T->End) {
        if (*T->p == '#') {
            while (T->p < T->End && *T->p != '\n') {
                T->p++;
            }
        } else if (*T->p == ' ' || *T->p == '\t' || *T->p == '\r' || *T->p == '\n') {
            T->p++;
        } else {
            break;
        }
    }
}

static BOOL
IsWordChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '+' || c == '-';
}

/* a quoted string, unescaped into the arena; T->p is on the opening quote */
static const char *
TextQuoted(LVM_TEXT *T)
{
    const char *Start = ++T->p;
    char       *Out, *o;

    while (T->p < T->End && *T->p != '"') {
        if (*T->p == '\\') {
            T->p++;
        }
        T->p++;
    }
    if (T->p >= T->End) {
        return NULL;
    }
    Out = o = (char *)TextTake(T, (size_t)(T->p - Start) + 1);
    if (Out == NULL) {
        return NULL;
    }
    for (const char *s = Start; s < T->p; s++) {
        if (*s == '\\' && s + 1 < T->p) {
            s++;
        }
        *o++ = *s;
    }
    T->p++;
    return Out;
}

/* a word or a quoted string, copied into the arena */
static const char *
TextToken(LVM_TEXT *T)
{
    const char *Start;
    char       *Out;

    TextSkip(T);
    if (T->p >= T->End) {
        return NULL;
    }
    if (*T->p == '"') {
        return TextQuoted(T);
    }
    Start = T->p;
    while (T->p < T->End && IsWordChar(*T->p)) {
        T->p++;
    }
    if (T->p == Start) {
        return NULL;
    }
    Out = (char *)TextTake(T, (size_t)(T->p - Start) + 1);
    if (Out) {
        memcpy(Out, Start, (size_t)(T->p - Start));
    }
    return Out;
}

/* the items of a list after its '[', up to and past its ']' */
static BOOL
TextList(LVM_TEXT *T, LVM_NODE *List)
{
    LVM_NODE **Tail = &List->Child;

    List->Kind = LVM_LIST;
    for (;;) {
        LVM_NODE *Item;

        TextSkip(T);
        if (T->p < T->End && *T->p == ']') {
            T->p++;
            return TRUE;
        }
        if (T->p < T->End && *T->p == ',') {
            T->p++;
            continue;
        }
        Item = (LVM_NODE *)TextTake(T, sizeof(LVM_NODE));
        if (Item == NULL || (Item->Value = TextToken(T)) == NULL) {
            return FALSE;
        }
        Item->Kind = LVM_VALUE;
        *Tail = Item;
        Tail = &Item->Next;
    }
}

/* the members of a section up to '}' (or the end of the text at the top) */
static LVM_NODE *
TextSection(LVM_TEXT *T, BOOL Top)
{
    LVM_NODE *First = NULL, **Tail = &First;

    if (++T->Depth > LVM_MAX_DEPTH) {
        return NULL;
    }
    for (;;) {
        const char *Name;
        LVM_NODE   *Node;

        TextSkip(T);
        if (T->p >= T->End) {
            if (!Top) {
                return NULL;
            }
            break;
        }
        if (*T->p == '}') {
            if (Top) {
                return NULL;
            }
            T->p++;
            break;
        }
        Name = TextToken(T);
        if (Name == NULL || (Node = (LVM_NODE *)TextTake(T, sizeof(LVM_NODE))) == NULL) {
            return NULL;
        }
        Node->Name = Name;
        TextSkip(T);
        if (T->p < T->End && *T->p == '{') {
            T->p++;
            Node->Kind = LVM_SECTION;
            Node->Child = TextSection(T, FALSE);
            if (Node->Child == NULL && T->p > T->End) {
                return NULL;
            }
        } else if (T->p < T->End && *T->p == '=') {
            T->p++;
            TextSkip(T);
            if (T->p < T->End && *T->p == '[') {
                T->p++;
                if (!TextList(T, Node)) {
                    return NULL;
                }
            } else {
                Node->Kind = LVM_VALUE;
                if ((Node->Value = TextToken(T)) == NULL) {
                    return NULL;
                }
            }
        } else {
            return NULL;
        }
        *Tail = Node;
        Tail = &Node->Next;
    }
    T->Depth--;
    return First ? First : (LVM_NODE *)TextTake(T, sizeof(LVM_NODE));   /* empty section: a dummy */
}

BOOL
LvmParseText(LVM_TEXT *T, const char *Text, size_t Length)
{
    memset(T, 0, sizeof(*T));
    T->Size = Length * LVM_ARENA_FACTOR + LVM_ARENA_EXTRA;
    T->Arena = (char *)HeapAlloc(GetProcessHeap(), 0, T->Size);
    if (T->Arena == NULL) {
        return FALSE;
    }
    T->p = Text;
    T->End = Text + strnlen(Text, Length);
    T->Root = (LVM_NODE *)TextTake(T, sizeof(LVM_NODE));
    if (T->Root == NULL) {
        return FALSE;
    }
    T->Root->Kind = LVM_SECTION;
    T->Root->Child = TextSection(T, TRUE);
    return TRUE;
}

void
LvmFreeText(LVM_TEXT *T)
{
    if (T->Arena) {
        HeapFree(GetProcessHeap(), 0, T->Arena);
    }
    memset(T, 0, sizeof(*T));
}

/* ---------------------------------------------------------------- lookups */

const LVM_NODE *
LvmGet(const LVM_NODE *Section, const char *Name)
{
    const LVM_NODE *n;

    for (n = Section ? Section->Child : NULL; n; n = n->Next) {
        if (n->Name && strcmp(n->Name, Name) == 0) {
            return n;
        }
    }
    return NULL;
}

/* digits only, as LVM writes its numbers */
BOOL
LvmDecimal(const char *Text, UINT64 *Out)
{
    UINT64      v = 0;
    const char *s;

    if (Text == NULL || *Text == 0) {
        return FALSE;
    }
    for (s = Text; *s; s++) {
        if (*s < '0' || *s > '9') {
            return FALSE;
        }
        v = v * 10 + (UINT64)(*s - '0');
    }
    *Out = v;
    return TRUE;
}

BOOL
LvmU64(const LVM_NODE *Section, const char *Name, UINT64 *Out)
{
    const LVM_NODE *n = LvmGet(Section, Name);

    return n != NULL && n->Kind == LVM_VALUE && LvmDecimal(n->Value, Out);
}

const char *
LvmStr(const LVM_NODE *Section, const char *Name)
{
    const LVM_NODE *n = LvmGet(Section, Name);

    return (n && n->Kind == LVM_VALUE) ? n->Value : NULL;
}

/* is Flag among the items of list Name ("status" = ["READ", "VISIBLE"]) */
BOOL
LvmHasFlag(const LVM_NODE *Section, const char *Name, const char *Flag)
{
    const LVM_NODE *l = LvmGet(Section, Name), *i;

    for (i = (l && l->Kind == LVM_LIST) ? l->Child : NULL; i; i = i->Next) {
        if (i->Value && strcmp(i->Value, Flag) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

/* "segment1", "segment2" ... of a logical volume */
BOOL
LvmIsSegment(const LVM_NODE *Node)
{
    static const char Prefix[] = "segment";

    return Node->Kind == LVM_SECTION && strncmp(Node->Name, Prefix, sizeof(Prefix) - 1) == 0;
}

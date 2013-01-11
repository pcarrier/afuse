/**
 * @file variable_pairing_heap.h
 *
 * @author Jeremy Maitin-Shepard <jeremy@jeremyms.com>
 *
 * This file defines a type-generic pairing heap implementation as
 * several macros that generate the implementation code.
 */

#ifndef _VARIABLE_PAIRING_HEAP_H
#define _VARIABLE_PAIRING_HEAP_H

#define PH_NEW_LINK(PH_ELEM_TYPE)               \
  struct                                        \
  {                                             \
    PH_ELEM_TYPE *child, *next, *prev;          \
  }

#define PH_DECLARE_TYPE(PH_PREFIX, PH_ELEM_TYPE)                        \
  typedef PH_ELEM_TYPE *PH_PREFIX ## _t;                                \
  void PH_PREFIX ## _init(PH_PREFIX ## _t *ph);                         \
  PH_ELEM_TYPE *PH_PREFIX ## _min(PH_PREFIX ## _t *ph);                 \
  void PH_PREFIX ## _insert(PH_PREFIX ## _t *ph, PH_ELEM_TYPE *elem);   \
  void PH_PREFIX ## _remove(PH_PREFIX ## _t *ph, PH_ELEM_TYPE *elem);   \
  void PH_PREFIX ## _remove_min(PH_PREFIX ## _t *ph);

#define PH_DEFINE_TYPE(PH_PREFIX, PH_ELEM_TYPE, PH_NODE_NAME, PH_KEY_NAME)     \
  void PH_PREFIX ## _init(PH_PREFIX ## _t *ph)                                 \
  {                                                                            \
    *ph = NULL;                                                                \
  }                                                                            \
  PH_ELEM_TYPE *PH_PREFIX ## _min(PH_PREFIX ## _t *ph)                         \
  {                                                                            \
    return *ph;                                                                \
  }                                                                            \
  static PH_ELEM_TYPE *PH_PREFIX ## _meld(PH_ELEM_TYPE *a,                     \
                                          PH_ELEM_TYPE *b)                     \
  {                                                                            \
    if (!b)                                                                    \
      return a;                                                                \
    if (a->PH_KEY_NAME > b->PH_KEY_NAME)                                       \
    {                                                                          \
      PH_ELEM_TYPE *temp = a;                                                  \
      a = b;                                                                   \
      b = temp;                                                                \
    }                                                                          \
    b->PH_NODE_NAME.next = a->PH_NODE_NAME.child;                              \
    b->PH_NODE_NAME.prev = a;                                                  \
    if (a->PH_NODE_NAME.child)                                                 \
      a->PH_NODE_NAME.child->PH_NODE_NAME.prev = b;                            \
    a->PH_NODE_NAME.child = b;                                                 \
    return a;                                                                  \
  }                                                                            \
  void PH_PREFIX ## _insert(PH_PREFIX ## _t *ph, PH_ELEM_TYPE *elem)           \
  {                                                                            \
    elem->PH_NODE_NAME.child = NULL;                                           \
    elem->PH_NODE_NAME.next = NULL;                                            \
    elem->PH_NODE_NAME.prev = NULL;                                            \
    *ph = PH_PREFIX ## _meld(elem, *ph);                                       \
  }                                                                            \
  static PH_ELEM_TYPE *PH_PREFIX ## _combine_children(PH_ELEM_TYPE *ph)        \
  {                                                                            \
    PH_ELEM_TYPE *head = NULL, *tail = NULL, *cur = ph->PH_NODE_NAME.child,    \
      *next, *nnext, *m = NULL;                                                \
    if (!cur)                                                                  \
      return NULL;                                                             \
    while (1)                                                                  \
    {                                                                          \
      if (!cur->PH_NODE_NAME.next)                                             \
        next = NULL;                                                           \
      else                                                                     \
        next = cur->PH_NODE_NAME.next->PH_NODE_NAME.next;                      \
      m = PH_PREFIX ## _meld(cur, cur->PH_NODE_NAME.next);                     \
      if (tail)                                                                \
        tail->PH_NODE_NAME.next = m;                                           \
      else                                                                     \
        head = m;                                                              \
      tail = m;                                                                \
      if (!next)                                                               \
        break;                                                                 \
      cur = next;                                                              \
    }                                                                          \
    while (head != tail)                                                       \
    {                                                                          \
      next = head->PH_NODE_NAME.next;                                          \
      nnext = next->PH_NODE_NAME.next;                                         \
      m = PH_PREFIX ## _meld(head, next);                                      \
      if (next == tail)                                                        \
        break;                                                                 \
      tail->PH_NODE_NAME.next = m;                                             \
      tail = m;                                                                \
      head = nnext;                                                            \
    }                                                                          \
    m->PH_NODE_NAME.prev = NULL;                                               \
    m->PH_NODE_NAME.next = NULL;                                               \
    return m;                                                                  \
  }                                                                            \
  void PH_PREFIX ## _remove_min(PH_PREFIX ## _t *ph)                           \
  {                                                                            \
    *ph = PH_PREFIX ## _combine_children(*ph);                                 \
  }                                                                            \
  void PH_PREFIX ## _remove(PH_PREFIX ## _t *ph, PH_ELEM_TYPE *elem)           \
  {                                                                            \
    if (elem == *ph)                                                           \
      PH_PREFIX ## _remove_min(ph);                                            \
    else                                                                       \
    {                                                                          \
      PH_ELEM_TYPE *prev = elem->PH_NODE_NAME.prev;                            \
      if (prev->PH_NODE_NAME.child == elem)                                    \
        prev->PH_NODE_NAME.child = elem->PH_NODE_NAME.next;                    \
      else                                                                     \
        prev->PH_NODE_NAME.next = elem->PH_NODE_NAME.next;                     \
      if (elem->PH_NODE_NAME.next)                                             \
        elem->PH_NODE_NAME.next->PH_NODE_NAME.prev = prev;                     \
      *ph = PH_PREFIX ## _meld                                                 \
        (*ph, PH_PREFIX ## _combine_children(elem));                           \
    }                                                                          \
  }

#endif				/* _VARIABLE_PAIRING_HEAP_H */

#pragma once

/*
 * This is largely generic for little-endian machines, but the
 * optimal byte mask counting is probably going to be something
 * that is architecture-specific. If you have a reliably fast
 * bit count instruction, that might be better than the multiply
 * and shift, for example.
 */
struct word_at_a_time {
        const unsigned long one_bits, high_bits;
};

#define REPEAT_BYTE(x)  ((~0ul / 0xff) * (x))

#define WORD_AT_A_TIME_CONSTANTS { REPEAT_BYTE(0x01), REPEAT_BYTE(0x80) }

/*
 * Jan Achrenius on G+: microoptimized version of
 * the simpler "(mask & ONEBYTES) * ONEBYTES >> 56"
 * that works for the bytemasks without having to
 * mask them first.
 */
static inline long count_masked_bytes(unsigned long mask)
{
        return mask*0x0001020304050608ul >> 56;
}

/* Return nonzero if it has a zero */
static inline unsigned long has_zero(unsigned long a, unsigned long *bits, const struct word_at_a_time *c)
{
        unsigned long mask = ((a - c->one_bits) & ~a) & c->high_bits;
        *bits = mask;
        return mask;
}

static inline unsigned long prep_zero_mask(unsigned long a, unsigned long bits, const struct word_at_a_time *c)
{
        return bits;
}

static inline unsigned long create_zero_mask(unsigned long bits)
{
        bits = (bits - 1) & ~bits;
        return bits >> 7;
}

/* The mask we created is directly usable as a bytemask */
#define zero_bytemask(mask) (mask)

static inline unsigned long find_zero(unsigned long mask)
{
        return count_masked_bytes(mask);
}

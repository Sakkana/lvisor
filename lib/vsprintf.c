#include <sys/ctype.h>
#include <sys/printk.h>
#include <sys/string.h>

static noinline int skip_atoi(const char **s)
{
        int i = 0;

        do {
                i = i*10 + *((*s)++) - '0';
        } while (isdigit(**s));

        return i;
}

/*
 * Decimal conversion is by far the most typical, and is used for
 * /proc and /sys data. This directly impacts e.g. top performance
 * with many processes running. We optimize it for speed by emitting
 * two characters at a time, using a 200 byte lookup table. This
 * roughly halves the number of multiplications compared to computing
 * the digits one at a time. Implementation strongly inspired by the
 * previous version, which in turn used ideas described at
 * <http://www.cs.uiowa.edu/~jones/bcd/divide.html> (with permission
 * from the author, Douglas W. Jones).
 *
 * It turns out there is precisely one 26 bit fixed-point
 * approximation a of 64/100 for which x/100 == (x * (u64)a) >> 32
 * holds for all x in [0, 10^8-1], namely a = 0x28f5c29. The actual
 * range happens to be somewhat larger (x <= 1073741898), but that's
 * irrelevant for our purpose.
 *
 * For dividing a number in the range [10^4, 10^6-1] by 100, we still
 * need a 32x32->64 bit multiply, so we simply use the same constant.
 *
 * For dividing a number in the range [100, 10^4-1] by 100, there are
 * several options. The simplest is (x * 0x147b) >> 19, which is valid
 * for all x <= 43698.
 */

static const uint16_t decpair[100] = {
#define _(x) (uint16_t) (((x % 10) | ((x / 10) << 8)) + 0x3030)
        _( 0), _( 1), _( 2), _( 3), _( 4), _( 5), _( 6), _( 7), _( 8), _( 9),
        _(10), _(11), _(12), _(13), _(14), _(15), _(16), _(17), _(18), _(19),
        _(20), _(21), _(22), _(23), _(24), _(25), _(26), _(27), _(28), _(29),
        _(30), _(31), _(32), _(33), _(34), _(35), _(36), _(37), _(38), _(39),
        _(40), _(41), _(42), _(43), _(44), _(45), _(46), _(47), _(48), _(49),
        _(50), _(51), _(52), _(53), _(54), _(55), _(56), _(57), _(58), _(59),
        _(60), _(61), _(62), _(63), _(64), _(65), _(66), _(67), _(68), _(69),
        _(70), _(71), _(72), _(73), _(74), _(75), _(76), _(77), _(78), _(79),
        _(80), _(81), _(82), _(83), _(84), _(85), _(86), _(87), _(88), _(89),
        _(90), _(91), _(92), _(93), _(94), _(95), _(96), _(97), _(98), _(99),
#undef _
};

/*
 * This will print a single '0' even if r == 0, since we would
 * immediately jump to out_r where two 0s would be written but only
 * one of them accounted for in buf. This is needed by ip4_string
 * below. All other callers pass a non-zero value of r.
*/
static noinline char *put_dec_trunc8(char *buf, unsigned r)
{
        unsigned q;

        /* 1 <= r < 10^8 */
        if (r < 100)
                goto out_r;

        /* 100 <= r < 10^8 */
        q = (r * UINT64_C(0x28f5c29)) >> 32;
        *((uint16_t *)buf) = decpair[r - 100*q];
        buf += 2;

        /* 1 <= q < 10^6 */
        if (q < 100)
                goto out_q;

        /*  100 <= q < 10^6 */
        r = (q * UINT64_C(0x28f5c29)) >> 32;
        *((uint16_t *)buf) = decpair[q - 100*r];
        buf += 2;

        /* 1 <= r < 10^4 */
        if (r < 100)
                goto out_r;

        /* 100 <= r < 10^4 */
        q = (r * 0x147b) >> 19;
        *((uint16_t *)buf) = decpair[r - 100*q];
        buf += 2;
out_q:
        /* 1 <= q < 100 */
        r = q;
out_r:
        /* 1 <= r < 100 */
        *((uint16_t *)buf) = decpair[r];
        buf += r < 10 ? 1 : 2;
        return buf;
}

static noinline char *put_dec_full8(char *buf, unsigned r)
{
        unsigned q;

        /* 0 <= r < 10^8 */
        q = (r * UINT64_C(0x28f5c29)) >> 32;
        *((uint16_t *)buf) = decpair[r - 100*q];
        buf += 2;

        /* 0 <= q < 10^6 */
        r = (q * UINT64_C(0x28f5c29)) >> 32;
        *((uint16_t *)buf) = decpair[q - 100*r];
        buf += 2;

        /* 0 <= r < 10^4 */
        q = (r * 0x147b) >> 19;
        *((uint16_t *)buf) = decpair[r - 100*q];
        buf += 2;

        /* 0 <= q < 100 */
        *((uint16_t *)buf) = decpair[q];
        buf += 2;
        return buf;
}

static noinline char *put_dec(char *buf, unsigned long long n)
{
        if (n >= 100*1000*1000)
                buf = put_dec_full8(buf, do_div(n, (100*1000*1000)));
        /* 1 <= n <= 1.6e11 */
        if (n >= 100*1000*1000)
                buf = put_dec_full8(buf, do_div(n, (100*1000*1000)));
        /* 1 <= n < 1e8 */
        return put_dec_trunc8(buf, n);
}

#define SIGN    1               /* unsigned/signed, must be 1 */
#define LEFT    2               /* left justified */
#define PLUS    4               /* show plus */
#define SPACE   8               /* space if plus */
#define ZEROPAD 16              /* pad with zero, must be 16 == '0' - ' ' */
#define SMALL   32              /* use lowercase in hex (must be 32 == 0x20) */
#define SPECIAL 64              /* prefix hex with "0x", octal with "0" */

enum format_type {
        FORMAT_TYPE_NONE, /* Just a string part */
        FORMAT_TYPE_WIDTH,
        FORMAT_TYPE_PRECISION,
        FORMAT_TYPE_CHAR,
        FORMAT_TYPE_STR,
        FORMAT_TYPE_PTR,
        FORMAT_TYPE_PERCENT_CHAR,
        FORMAT_TYPE_INVALID,
        FORMAT_TYPE_LONG_LONG,
        FORMAT_TYPE_ULONG,
        FORMAT_TYPE_LONG,
        FORMAT_TYPE_UBYTE,
        FORMAT_TYPE_BYTE,
        FORMAT_TYPE_USHORT,
        FORMAT_TYPE_SHORT,
        FORMAT_TYPE_UINT,
        FORMAT_TYPE_INT,
        FORMAT_TYPE_SIZE_T,
        FORMAT_TYPE_PTRDIFF
};

struct printf_spec {
        unsigned int    type:8;         /* format_type enum */
        signed int      field_width:24; /* width of output field */
        unsigned int    flags:8;        /* flags to number() */
        unsigned int    base:8;         /* number base, 8, 10 or 16 only */
        signed int      precision:16;   /* # of digits/chars */
} __packed;

static noinline char *number(char *buf, char *end, unsigned long long num, struct printf_spec spec)
{
        /* put_dec requires 2-byte alignment of the buffer. */
        char tmp[3 * sizeof(num)] __aligned(2);
        char sign;
        char locase;
        int need_pfx = ((spec.flags & SPECIAL) && spec.base != 10);
        int i;
        bool is_zero = num == 0LL;
        int field_width = spec.field_width;
        int precision = spec.precision;

        /* locase = 0 or 0x20. ORing digits or letters with 'locase'
         * produces same digits or (maybe lowercased) letters */
        locase = (spec.flags & SMALL);
        if (spec.flags & LEFT)
                spec.flags &= ~ZEROPAD;
        sign = 0;
        if (spec.flags & SIGN) {
                if ((signed long long)num < 0) {
                        sign = '-';
                        num = -(signed long long)num;
                        field_width--;
                } else if (spec.flags & PLUS) {
                        sign = '+';
                        field_width--;
                } else if (spec.flags & SPACE) {
                        sign = ' ';
                        field_width--;
                }
        }
        if (need_pfx) {
                if (spec.base == 16)
                        field_width -= 2;
                else if (!is_zero)
                        field_width--;
        }

        /* generate full string in tmp[], in reverse order */
        i = 0;
        if (num < spec.base)
                tmp[i++] = hex_asc_upper[num] | locase;
        else if (spec.base != 10) { /* 8 or 16 */
                int mask = spec.base - 1;
                int shift = 3;

                if (spec.base == 16)
                        shift = 4;
                do {
                        tmp[i++] = (hex_asc_upper[((unsigned char)num) & mask] | locase);
                        num >>= shift;
                } while (num);
        } else { /* base 10 */
                i = put_dec(tmp, num) - tmp;
        }

        /* printing 100 using %2d gives "100", not "00" */
        if (i > precision)
                precision = i;
        /* leading space padding */
        field_width -= precision;
        if (!(spec.flags & (ZEROPAD | LEFT))) {
                while (--field_width >= 0) {
                        if (buf < end)
                                *buf = ' ';
                        ++buf;
                }
        }
        /* sign */
        if (sign) {
                if (buf < end)
                        *buf = sign;
                ++buf;
        }
        /* "0x" / "0" prefix */
        if (need_pfx) {
                if (spec.base == 16 || !is_zero) {
                        if (buf < end)
                                *buf = '0';
                        ++buf;
                }
                if (spec.base == 16) {
                        if (buf < end)
                                *buf = ('X' | locase);
                        ++buf;
                }
        }
        /* zero or space padding */
        if (!(spec.flags & LEFT)) {
                char c = ' ' + (spec.flags & ZEROPAD);
                while (--field_width >= 0) {
                        if (buf < end)
                                *buf = c;
                        ++buf;
                }
        }
        /* hmm even more zero padding? */
        while (i <= --precision) {
                if (buf < end)
                        *buf = '0';
                ++buf;
        }
        /* actual digits of result */
        while (--i >= 0) {
                if (buf < end)
                        *buf = tmp[i];
                ++buf;
        }
        /* trailing space padding */
        while (--field_width >= 0) {
                if (buf < end)
                        *buf = ' ';
                ++buf;
        }

        return buf;
}

static noinline char *special_hex_number(char *buf, char *end, unsigned long long num, int size)
{
        struct printf_spec spec;

        spec.type = FORMAT_TYPE_PTR;
        spec.field_width = 2 + 2 * size;	/* 0x + hex */
        spec.flags = SPECIAL | SMALL | ZEROPAD;
        spec.base = 16;
        spec.precision = -1;

        return number(buf, end, num, spec);
}

static void move_right(char *buf, char *end, unsigned len, unsigned spaces)
{
        size_t size;
        if (buf >= end) /* nowhere to put anything */
                return;
        size = end - buf;
        if (size <= spaces) {
                memset(buf, ' ', size);
                return;
        }
        if (len) {
                if (len > size - spaces)
                        len = size - spaces;
                memmove(buf + spaces, buf, len);
        }
        memset(buf, ' ', spaces);
}

/*
 * Handle field width padding for a string.
 * @buf: current buffer position
 * @n: length of string
 * @end: end of output buffer
 * @spec: for field width and flags
 * Returns: new buffer position after padding.
 */
static noinline char *widen_string(char *buf, int n, char *end, struct printf_spec spec)
{
        unsigned spaces;

        if (n >= spec.field_width)
                return buf;
        /* we want to pad the sucker */
        spaces = spec.field_width - n;
        if (!(spec.flags & LEFT)) {
                move_right(buf - n, end, n, spaces);
                return buf + spaces;
        }
        while (spaces--) {
                if (buf < end)
                        *buf = ' ';
                ++buf;
        }
        return buf;
}

static noinline char *string(char *buf, char *end, const char *s, struct printf_spec spec)
{
        int len = 0;
        size_t lim = spec.precision;

        if (!s)
                s = "(null)";

        while (lim--) {
                char c = *s++;
                if (!c)
                        break;
                if (buf < end)
                        *buf = c;
                ++buf;
                ++len;
        }
        return widen_string(buf, len, end, spec);
}

static noinline char *symbol_string(char *buf, char *end, void *ptr,
                                    struct printf_spec spec, const char *fmt)
{
        uintptr_t value;

        if (fmt[1] == 'R')
                ptr = __builtin_extract_return_addr(ptr);
        value = (uintptr_t)ptr;

        return special_hex_number(buf, end, value, sizeof(void *));
}

static noinline char *hex_string(char *buf, char *end, uint8_t *addr,
                                 struct printf_spec spec, const char *fmt)
{
        int i, len = 1;         /* if we pass '%ph[CDN]', field width remains
                                   negative value, fallback to the default */
        char separator;

        if (spec.field_width == 0)
                /* nothing to print */
                return buf;

        /* NULL pointer */
        if (!addr)
                return string(buf, end, NULL, spec);

        switch (fmt[1]) {
        case 'C':
                separator = ':';
                break;
        case 'D':
                separator = '-';
                break;
        case 'N':
                separator = 0;
                break;
        default:
                separator = ' ';
                break;
        }

        if (spec.field_width > 0)
                len = min_t(int, spec.field_width, 64);

        for (i = 0; i < len; ++i) {
                if (buf < end)
                        *buf = hex_asc_hi(addr[i]);
                ++buf;
                if (buf < end)
                        *buf = hex_asc_lo(addr[i]);
                ++buf;

                if (separator && i != len - 1) {
                        if (buf < end)
                                *buf = separator;
                        ++buf;
                }
        }

        return buf;
}

static noinline char *mac_address_string(char *buf, char *end, uint8_t *addr,
                                         struct printf_spec spec, const char *fmt)
{
        char mac_addr[sizeof("xx:xx:xx:xx:xx:xx")];
        char *p = mac_addr;
        int i;
        char separator;
        bool reversed = false;

        switch (fmt[1]) {
        case 'F':
                separator = '-';
                break;

        case 'R':
                reversed = true;
                /* fall through */

        default:
                separator = ':';
                break;
        }

        for (i = 0; i < 6; i++) {
                if (reversed)
                        p = hex_byte_pack(p, addr[5 - i]);
                else
                        p = hex_byte_pack(p, addr[i]);

                if (fmt[0] == 'M' && i != 5)
                        *p++ = separator;
        }
        *p = '\0';

        return string(buf, end, mac_addr, spec);
}

static noinline char *pointer_string(char *buf, char *end, const void *ptr,
                                     struct printf_spec spec)
{
        spec.base = 16;
        spec.flags |= SMALL;
        if (spec.field_width == -1) {
                spec.field_width = 2 * sizeof(ptr);
                spec.flags |= ZEROPAD;
        }

        return number(buf, end, (uintptr_t)ptr, spec);
}

/*
 * Show a '%p' thing.  A kernel extension is that the '%p' is followed
 * by an extra set of alphanumeric characters that are extended format
 * specifiers.
 *
 * Right now we handle:
 *
 * - 'F' For symbolic function descriptor pointers with offset
 * - 'f' For simple symbolic function names without offset
 * - 'S' For symbolic direct pointers with offset
 * - 's' For symbolic direct pointers without offset
 * - '[FfSs]R' as above with __builtin_extract_return_addr() translation
 * - 'B' For backtraced symbolic direct pointers with offset
 * - 'M' For a 6-byte MAC address, it prints the address in the
 *       usual colon-separated hex notation
 * - 'm' For a 6-byte MAC address, it prints the hex address without colons
 * - 'MF' For a 6-byte MAC FDDI address, it prints the address
 *       with a dash-separated hex notation
 * - '[mM]R' For a 6-byte MAC address, Reverse order (Bluetooth)
 * - 'h[CDN]' For a variable-length buffer, it prints it as a hex string with
 *            a certain separator (' ' by default):
 *              C colon
 *              D dash
 *              N no separator
 *            The maximum supported length is 64 bytes of the input. Consider
 *            to use print_hex_dump() for the larger input.
 *
 * - 'x' For printing the address. Equivalent to "%lx".
 */
static noinline char *pointer(const char *fmt, char *buf, char *end, void *ptr, struct printf_spec spec)
{
        const int default_width = 2 * sizeof(void *);

        if (!ptr) {
                /*
                 * Print (null) with the same width as a pointer so it makes
                 * tabular output look nice.
                 */
                if (spec.field_width == -1)
                        spec.field_width = default_width;
                return string(buf, end, "(null)", spec);
        }

        switch (*fmt) {
        case 'F':
        case 'f':
        case 'S':
        case 's':
        case 'B':
                return symbol_string(buf, end, ptr, spec, fmt);
        case 'h':
                return hex_string(buf, end, ptr, spec, fmt);
        case 'M':                       /* Colon separated: 00:01:02:03:04:05 */
        case 'm':                       /* Contiguous: 000102030405 */
                                        /* [mM]F (FDDI) */
                                        /* [mM]R (Reverse order; Bluetooth) */
                return mac_address_string(buf, end, ptr, spec, fmt);
        default:
                break;
        }

        return pointer_string(buf, end, ptr, spec);
}

/*
 * Helper function to decode printf style format.
 * Each call decode a token from the format and return the
 * number of characters read (or likely the delta where it wants
 * to go on the next call).
 * The decoded token is returned through the parameters
 *
 * 'h', 'l', or 'L' for integer fields
 * 'z' support added 23/7/1999 S.H.
 * 'z' changed to 'Z' --davidm 1/25/99
 * 'Z' changed to 'z' --adobriyan 2017-01-25
 * 't' added for ptrdiff_t
 *
 * @fmt: the format string
 * @type of the token returned
 * @flags: various flags such as +, -, # tokens..
 * @field_width: overwritten width
 * @base: base of the number (octal, hex, ...)
 * @precision: precision of a number
 * @qualifier: qualifier of a number (long, size_t, ...)
 */
static noinline int format_decode(const char *fmt, struct printf_spec *spec)
{
        const char *start = fmt;
        char qualifier;

        /* we finished early by reading the field width */
        if (spec->type == FORMAT_TYPE_WIDTH) {
                if (spec->field_width < 0) {
                        spec->field_width = -spec->field_width;
                        spec->flags |= LEFT;
                }
                spec->type = FORMAT_TYPE_NONE;
                goto precision;
        }

        /* we finished early by reading the precision */
        if (spec->type == FORMAT_TYPE_PRECISION) {
                if (spec->precision < 0)
                        spec->precision = 0;

                spec->type = FORMAT_TYPE_NONE;
                goto qualifier;
        }

        /* By default */
        spec->type = FORMAT_TYPE_NONE;

        for (; *fmt ; ++fmt) {
                if (*fmt == '%')
                        break;
        }

        /* Return the current non-format string */
        if (fmt != start || !*fmt)
                return fmt - start;

        /* Process flags */
        spec->flags = 0;

        while (1) { /* this also skips first '%' */
                bool found = true;

                ++fmt;

                switch (*fmt) {
                case '-': spec->flags |= LEFT;    break;
                case '+': spec->flags |= PLUS;    break;
                case ' ': spec->flags |= SPACE;   break;
                case '#': spec->flags |= SPECIAL; break;
                case '0': spec->flags |= ZEROPAD; break;
                default:  found = false;
                }

                if (!found)
                        break;
        }

        /* get field width */
        spec->field_width = -1;

        if (isdigit(*fmt))
                spec->field_width = skip_atoi(&fmt);
        else if (*fmt == '*') {
                /* it's the next argument */
                spec->type = FORMAT_TYPE_WIDTH;
                return ++fmt - start;
        }

precision:
        /* get the precision */
        spec->precision = -1;
        if (*fmt == '.') {
                ++fmt;
                if (isdigit(*fmt)) {
                        spec->precision = skip_atoi(&fmt);
                        if (spec->precision < 0)
                                spec->precision = 0;
                } else if (*fmt == '*') {
                        /* it's the next argument */
                        spec->type = FORMAT_TYPE_PRECISION;
                        return ++fmt - start;
                }
        }

qualifier:
        /* get the conversion qualifier */
        qualifier = 0;
        if (*fmt == 'h' || _tolower(*fmt) == 'l' ||
            *fmt == 'z' || *fmt == 't') {
                qualifier = *fmt++;
                if (qualifier == *fmt) {
                        if (qualifier == 'l') {
                                qualifier = 'L';
                                ++fmt;
                        } else if (qualifier == 'h') {
                                qualifier = 'H';
                                ++fmt;
                        }
                }
        }

        /* default base */
        spec->base = 10;
        switch (*fmt) {
        case 'c':
                spec->type = FORMAT_TYPE_CHAR;
                return ++fmt - start;

        case 's':
                spec->type = FORMAT_TYPE_STR;
                return ++fmt - start;

        case 'p':
                spec->type = FORMAT_TYPE_PTR;
                return ++fmt - start;

        case '%':
                spec->type = FORMAT_TYPE_PERCENT_CHAR;
                return ++fmt - start;

        /* integer number formats - set up the flags and "break" */
        case 'o':
                spec->base = 8;
                break;

        case 'x':
                spec->flags |= SMALL;

        case 'X':
                spec->base = 16;
                break;

        case 'd':
        case 'i':
                spec->flags |= SIGN;
        case 'u':
                break;

        case 'n':
                /*
                 * Since %n poses a greater security risk than
                 * utility, treat it as any other invalid or
                 * unsupported format specifier.
                 */
                /* Fall-through */

        default:
                spec->type = FORMAT_TYPE_INVALID;
                return fmt - start;
        }

        if (qualifier == 'L') {
                spec->type = FORMAT_TYPE_LONG_LONG;
        } else if (qualifier == 'l') {
                spec->type = FORMAT_TYPE_ULONG + (spec->flags & SIGN);
        } else if (qualifier == 'z') {
                spec->type = FORMAT_TYPE_SIZE_T;
        } else if (qualifier == 't') {
                spec->type = FORMAT_TYPE_PTRDIFF;
        } else if (qualifier == 'H') {
                spec->type = FORMAT_TYPE_UBYTE + (spec->flags & SIGN);
        } else if (qualifier == 'h') {
                spec->type = FORMAT_TYPE_USHORT + (spec->flags & SIGN);
        } else {
                spec->type = FORMAT_TYPE_UINT + (spec->flags & SIGN);
        }

        return ++fmt - start;
}

static void set_field_width(struct printf_spec *spec, int width)
{
        spec->field_width = width;
}

static void set_precision(struct printf_spec *spec, int prec)
{
        spec->precision = prec;
}

/**
 * vsnprintf - Format a string and place it in a buffer
 * @buf: The buffer to place the result into
 * @size: The size of the buffer, including the trailing null space
 * @fmt: The format string to use
 * @args: Arguments for the format string
 *
 * This function generally follows C99 vsnprintf, but has some
 * extensions and a few limitations:
 *
 * %n is unsupported
 * %p* is handled by pointer()
 *
 * See pointer() or Documentation/printk-formats.txt for more
 * extensive description.
 *
 * ** Please update the documentation in both places when making changes **
 *
 * The return value is the number of characters which would
 * be generated for the given input, excluding the trailing
 * '\0', as per ISO C99. If you want to have the exact
 * number of characters written into @buf as return value
 * (not including the trailing '\0'), use vscnprintf(). If the
 * return is greater than or equal to @size, the resulting
 * string is truncated.
 *
 * If you're not already dealing with a va_list consider using snprintf().
 */
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
        unsigned long long num;
        char *str, *end;
        struct printf_spec spec = {0};

        /* Reject out-of-range values early.  Large positive sizes are
           used for unknown buffer sizes. */
        if (size > INT_MAX)
                return 0;

        str = buf;
        end = buf + size;

        /* Make sure end is always >= buf */
        if (end < buf) {
                end = ((void *)-1);
                size = end - buf;
        }

        while (*fmt) {
                const char *old_fmt = fmt;
                int read = format_decode(fmt, &spec);

                fmt += read;

                switch (spec.type) {
                case FORMAT_TYPE_NONE: {
                        int copy = read;
                        if (str < end) {
                                if (copy > end - str)
                                        copy = end - str;
                                memcpy(str, old_fmt, copy);
                        }
                        str += read;
                        break;
                }

                case FORMAT_TYPE_WIDTH:
                        set_field_width(&spec, va_arg(args, int));
                        break;

                case FORMAT_TYPE_PRECISION:
                        set_precision(&spec, va_arg(args, int));
                        break;

                case FORMAT_TYPE_CHAR: {
                        char c;

                        if (!(spec.flags & LEFT)) {
                                while (--spec.field_width > 0) {
                                        if (str < end)
                                                *str = ' ';
                                        ++str;

                                }
                        }
                        c = (unsigned char) va_arg(args, int);
                        if (str < end)
                                *str = c;
                        ++str;
                        while (--spec.field_width > 0) {
                                if (str < end)
                                        *str = ' ';
                                ++str;
                        }
                        break;
                }

                case FORMAT_TYPE_STR:
                        str = string(str, end, va_arg(args, char *), spec);
                        break;

                case FORMAT_TYPE_PTR:
                        str = pointer(fmt, str, end, va_arg(args, void *),
                                      spec);
                        while (isalnum(*fmt))
                                fmt++;
                        break;

                case FORMAT_TYPE_PERCENT_CHAR:
                        if (str < end)
                                *str = '%';
                        ++str;
                        break;

                case FORMAT_TYPE_INVALID:
                        /*
                         * Presumably the arguments passed gcc's type
                         * checking, but there is no safe or sane way
                         * for us to continue parsing the format and
                         * fetching from the va_list; the remaining
                         * specifiers and arguments would be out of
                         * sync.
                         */
                        goto out;

                default:
                        switch (spec.type) {
                        case FORMAT_TYPE_LONG_LONG:
                                num = va_arg(args, long long);
                                break;
                        case FORMAT_TYPE_ULONG:
                                num = va_arg(args, unsigned long);
                                break;
                        case FORMAT_TYPE_LONG:
                                num = va_arg(args, long);
                                break;
                        case FORMAT_TYPE_SIZE_T:
                                if (spec.flags & SIGN)
                                        num = va_arg(args, ssize_t);
                                else
                                        num = va_arg(args, size_t);
                                break;
                        case FORMAT_TYPE_PTRDIFF:
                                num = va_arg(args, ptrdiff_t);
                                break;
                        case FORMAT_TYPE_UBYTE:
                                num = (unsigned char) va_arg(args, int);
                                break;
                        case FORMAT_TYPE_BYTE:
                                num = (signed char) va_arg(args, int);
                                break;
                        case FORMAT_TYPE_USHORT:
                                num = (unsigned short) va_arg(args, int);
                                break;
                        case FORMAT_TYPE_SHORT:
                                num = (short) va_arg(args, int);
                                break;
                        case FORMAT_TYPE_INT:
                                num = (int) va_arg(args, int);
                                break;
                        default:
                                num = va_arg(args, unsigned int);
                        }

                        str = number(str, end, num, spec);
                }
        }

out:
        if (size > 0) {
                if (str < end)
                        *str = '\0';
                else
                        end[-1] = '\0';
        }

        /* the trailing null byte doesn't count towards the total */
        return str-buf;
}

/**
 * vscnprintf - Format a string and place it in a buffer
 * @buf: The buffer to place the result into
 * @size: The size of the buffer, including the trailing null space
 * @fmt: The format string to use
 * @args: Arguments for the format string
 *
 * The return value is the number of characters which have been written into
 * the @buf not including the trailing '\0'. If @size is == 0 the function
 * returns 0.
 *
 * If you're not already dealing with a va_list consider using scnprintf().
 *
 * See the vsnprintf() documentation for format string extensions over C99.
 */
int vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
        int i;

        i = vsnprintf(buf, size, fmt, args);

        if (i < size)
                return i;
        if (size != 0)
                return size - 1;
        return 0;
}

/**
 * scnprintf - Format a string and place it in a buffer
 * @buf: The buffer to place the result into
 * @size: The size of the buffer, including the trailing null space
 * @fmt: The format string to use
 * @...: Arguments for the format string
 *
 * The return value is the number of characters written into @buf not including
 * the trailing '\0'. If @size is == 0 the function returns 0.
 */

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
        va_list args;
        int i;

        va_start(args, fmt);
        i = vscnprintf(buf, size, fmt, args);
        va_end(args);

        return i;
}

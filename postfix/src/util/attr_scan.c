/*++
/* NAME
/*	attr_scan 3
/* SUMMARY
/*	recover attributes from byte stream
/* SYNOPSIS
/*	#include <attr_io.h>
/*
/*	int	attr_scan(fp, flags, type, name, ...)
/*	VSTREAM	fp;
/*	int	flags;
/*	int	type;
/*	char	*name;
/* DESCRIPTION
/*	attr_scan() takes zero or more (name, value) scalar or array
/*	attribute arguments, and recovers the attribute values from the
/*	byte stream that was generated by attr_print().
/*
/*	The input stream is formatted as follows, where (item)* stands
/*	for zero or more instances of the specified item, and where
/*	(item1 | item2) stands for choice:
/*
/* .in +5
/*	attr-list :== (simple-attr | list-attr)* newline
/* .br
/*	simple-attr :== attr-name colon attr-value newline
/* .br
/*	list-attr :== attr-name (colon attr-value)* newline
/* .br
/*	attr-name :== any base64 encoded string
/* .br
/*	attr-value :== any base64 encoded string
/* .br
/*	colon :== the ASCII colon character
/* .br
/*	newline :== the ASCII newline character
/* .in
/*
/*	All character values are 7-bit ASCII. All attribute names and
/*	attribute values are sent as base64-encoded strings. The
/*	formatting rules aim to make implementations in PERL and other
/*	non-C languages easy.
/*
/*	Attributes must be sent in the requested order as specified with
/*	the attr_scan() argument list. The input stream may contain
/*	additional attributes at any point in the input stream, including
/*	additional instances of requested attributes.
/*
/*	Additional attributes are silently skipped over, unless the
/*	ATTR_FLAG_EXTRA processing flag is specified (see below). This
/*	allows for some flexibility in the evolution of protocols while
/*	still providing the option of being strict where desirable.
/*
/*	Arguments:
/* .IP fp
/*	Stream to recover the attributes from.
/* .IP flags
/*	The bit-wise OR of zero or more of the following.
/* .RS
/* .IP ATTR_FLAG_MISSING
/*	Log a warning when the input attribute list terminates before all
/*	requested attributes are recovered. It is always an error when the
/*	input stream ends without the newline attribute list terminator.
/* .IP ATTR_FLAG_EXTRA
/*	Log a warning and stop attribute recovery when the input stream
/*	contains an attribute that was not requested.
/* .RE
/* .IP type
/*	The type determines the arguments that follow.
/* .RS
/* .IP "ATTR_TYPE_NUM (char *, int *)"
/*	This argument is followed by an attribute name and an integer pointer.
/*	This is used for recovering an integer attribute value.
/* .IP "ATTR_TYPE_STR (char *, VSTRING *)"
/*	This argument is followed by an attribute name and a VSTRING pointer.
/*	This is used for recovering a string attribute value.
/* .IP "ATTR_TYPE_NUM_ARRAY (char *, INTV *)"
/*	This argument is followed by an attribute name and an INTV pointer.
/*	This is used for recovering an integer array attribute value.
/* .IP "ATTR_TYPE_NUM_ARRAY (char *, ARGV *)"
/*	This argument is followed by an attribute name and an ARGV pointer.
/*	This is used for recovering a string array attribute value.
/* .IP ATTR_TYPE_END
/*	This terminates the requested attribute list.
/* .RE
/* DIAGNOSTICS
/*	The result value is the number of attributes that were successfully
/*	recovered from the input stream (an array-valued attribute counts
/*	as one attribute).
/*
/*	Panic: interface violation. All system call errors are fatal.
/* SEE ALSO
/*	attr_print(3) send attributes over byte stream.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <stdarg.h>
#include <stdio.h>

/* Utility library. */

#include <vstream.h>
#include <vstring.h>
#include <msg.h>
#include <argv.h>
#include <intv.h>
#include <attr_io.h>

/* Application specific. */

extern int var_line_limit;		/* XXX */

#define STR(x)	vstring_str(x)
#define LEN(x)	VSTRING_LEN(x)

/* attr_scan_string - pull a string from the input stream */

static int attr_scan_string(VSTREAM *fp, VSTRING *plain_buf, const char *context)
{
    VSTRING *base64_buf = 0;
    int     limit = var_line_limit * 5 / 4;
    int     ch;

    if (base64_buf == 0)
	base64_buf = vstring_alloc(10);

    VSTRING_RESET(base64_buf);
    while ((ch = VSTREAM_GETC(fp)) != ':' && ch != '\n') {
	if (ch == VSTREAM_EOF) {
	    msg_warn("premature end-of-input from %s while reading %s",
		     VSTREAM_PATH(fp), context);
	    return (-1);
	}
	if (LEN(base64_buf) > limit) {
	    msg_warn("string length > %d characters from %s while reading %s",
		     limit, VSTREAM_PATH(fp), context);
	    return (-1);
	}
	VSTRING_ADDCH(base64_buf, ch);
    }
    VSTRING_TERMINATE(base64_buf);
    vstring_strcpy(plain_buf, STR(base64_buf));
    if (msg_verbose)
	msg_info("%s: %s", context, STR(plain_buf));
    return (ch);
}

/* attr_scan_number - pull a number from the input stream */

static int attr_scan_number(VSTREAM *fp, unsigned *ptr, VSTRING *str_buf,
			            const char *context)
{
    char    junk = 0;
    int     ch;

    if ((ch = attr_scan_string(fp, str_buf, context)) < 0)
	return (-1);
    if (sscanf(STR(str_buf), "%u%c", ptr, &junk) != 1 || junk != 0) {
	msg_warn("malformed numerical data from %s while %s: %.100s",
		 VSTREAM_PATH(fp), context, STR(str_buf));
	return (-1);
    }
    return (ch);
}

/* attr_vscan - receive attribute list from stream */

int     attr_vscan(VSTREAM *fp, int flags, va_list ap)
{
    const char *myname = "attr_scan";
    static VSTRING *str_buf = 0;
    int     wanted_type;
    char   *wanted_name;
    int    *number;
    VSTRING *string;
    INTV   *number_array;
    ARGV   *string_array;
    unsigned num_val;
    int     ch;
    int     conversions = 0;

    /*
     * Initialize.
     */
    if (str_buf == 0)
	str_buf = vstring_alloc(10);

    /*
     * Iterate over all (type, name, value) triples.
     */
    for (;;) {

	/*
	 * Determine the next attribute name on the caller's wish list.
	 */
	wanted_type = va_arg(ap, int);
	if (wanted_type == ATTR_TYPE_END) {
	    wanted_name = "attribute list terminator";
	} else {
	    wanted_name = va_arg(ap, char *);
	}

	/*
	 * Locate the next attribute of interest in the input stream.
	 */
	for (;;) {

	    /*
	     * Get the name of the next attribute. Hitting the end-of-input
	     * early is OK if the caller is prepared to deal with missing
	     * inputs.
	     */
	    if ((ch = attr_scan_string(fp, str_buf,
				       "attribute name")) == VSTREAM_EOF)
		return (conversions);
	    if (ch == '\n' && LEN(str_buf) == 0) {
		if (wanted_type == ATTR_TYPE_END
		    || (flags & ATTR_FLAG_MISSING) == 0)
		    return (conversions);
		msg_warn("missing attribute %s in input from %s",
			 wanted_name, VSTREAM_PATH(fp));
		return (conversions);
	    }
	    if (msg_verbose)
		msg_info("want attribute %s, found attribute: %s",
			 wanted_name, STR(str_buf));

	    /*
	     * See if the caller asks for this attribute.
	     */
	    if (wanted_type != ATTR_TYPE_END
		&& strcmp(wanted_name, STR(str_buf)) == 0)
		break;
	    if ((flags & ATTR_FLAG_EXTRA) != 0) {
		msg_warn("spurious attribute %s in input from %s",
			 STR(str_buf), VSTREAM_PATH(fp));
		return (conversions);
	    }

	    /*
	     * Skip over this attribute. The caller does not ask for it.
	     */
	    while ((ch = VSTREAM_GETC(fp)) != VSTREAM_EOF && ch != '\n')
		 /* void */ ;
	}

	/*
	 * Do the requested conversion. If the target attribute is a
	 * non-array type, do not allow the sender to send a multi-valued
	 * attribute. If the target attribute is an array type, allow the
	 * sender to send a zero-element array.
	 */
	switch (wanted_type) {
	case ATTR_TYPE_NUM:
	    number = va_arg(ap, int *);
	    if ((ch = attr_scan_number(fp, number, str_buf,
				       "attribute value")) < 0)
		return (conversions);
	    if (ch != '\n') {
		msg_warn("too many values for attribute %s from %s",
			 wanted_name, VSTREAM_PATH(fp));
		return (conversions);
	    }
	    break;
	case ATTR_TYPE_STR:
	    string = va_arg(ap, VSTRING *);
	    if ((ch = attr_scan_string(fp, string, "attribute value")) < 0)
		return (conversions);
	    if (ch != '\n') {
		msg_warn("too many values for attribute %s from %s",
			 wanted_name, VSTREAM_PATH(fp));
		return (conversions);
	    }
	    break;
	case ATTR_TYPE_NUM_ARRAY:
	    number_array = va_arg(ap, INTV *);
	    while (ch != '\n') {
		if ((ch = attr_scan_number(fp, &num_val, str_buf,
					   "attribute value")) < 0)
		    return (conversions);
		intv_add(number_array, 1, num_val);
	    }
	    break;
	case ATTR_TYPE_STR_ARRAY:
	    string_array = va_arg(ap, ARGV *);
	    while (ch != '\n') {
		if ((ch = attr_scan_string(fp, str_buf, "attribute value")) < 0)
		    return (conversions);
		argv_add(string_array, STR(str_buf), (char *) 0);
	    }
	    break;
	default:
	    msg_panic("%s: unknown type code: %d", myname, wanted_type);
	}
	conversions++;
    }
}

/* attr_scan - read attribute list from stream */

int     attr_scan(VSTREAM *fp, int flags,...)
{
    va_list ap;
    int     ret;

    va_start(ap, flags);
    ret = attr_vscan(fp, flags, ap);
    va_end(ap);
    return (ret);
}

#ifdef TEST

 /*
  * Proof of concept test program.  Mirror image of the attr_scan test
  * program.
  */
#include <msg_vstream.h>

int     var_line_limit = 2048;

int     main(int unused_argc, char **used_argv)
{
    INTV   *intv = intv_alloc(1);
    ARGV   *argv = argv_alloc(1);
    VSTRING *str_val = vstring_alloc(1);
    int     int_val;
    int     ret;
    int     i;

    msg_verbose = 1;
    msg_vstream_init(used_argv[0], VSTREAM_ERR);
    if ((ret = attr_scan(VSTREAM_IN,
			 ATTR_FLAG_MISSING | ATTR_FLAG_EXTRA,
			 ATTR_TYPE_NUM, ATTR_NAME_NUM, &int_val,
			 ATTR_TYPE_STR, ATTR_NAME_STR, str_val,
			 ATTR_TYPE_NUM_ARRAY, ATTR_NAME_NUM_ARRAY, intv,
			 ATTR_TYPE_STR_ARRAY, ATTR_NAME_STR_ARRAY, argv,
			 ATTR_TYPE_END)) == 4) {
	vstream_printf("%s %d\n", ATTR_NAME_NUM, int_val);
	vstream_printf("%s %s\n", ATTR_NAME_STR, STR(str_val));
	vstream_printf("%s", ATTR_NAME_NUM_ARRAY);
	for (i = 0; i < intv->intc; i++)
	    vstream_printf(" %d", intv->intv[i]);
	vstream_printf("\n");
	vstream_printf("%s", ATTR_NAME_STR_ARRAY);
	for (i = 0; i < argv->argc; i++)
	    vstream_printf(" %s", argv->argv[i]);
	vstream_printf("\n");
    } else {
	vstream_printf("return: %d\n", ret);
    }
    if (vstream_fflush(VSTREAM_OUT) != 0)
	msg_fatal("write error: %m");
    return (0);
}

#endif
/*
 *	PostgreSQL type definitions for the INET and CIDR types.
 *
 *	src/backend/utils/adt/network.c
 *
 *	Jon Postel RIP 16 Oct 1998
 */

#include "postgres.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "access/hash.h"
#include "catalog/pg_type.h"
#include "common/ip.h"
#include "lib/hyperloglog.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inet.h"
#include "utils/sortsupport.h"

/* A few constants for the width in bits of certain values in inet/cidr
 * abbreviated keys. */
/* TODO: Should I bother with these constants? They're good in that they're
 * used multiple times, but they arguably *reduce* clarity instead of
 * contributing to it. */
#define ABBREV_BITS_INET_FAMILY 1
#if SIZEOF_DATUM == 8
#define ABBREV_BITS_INET4_NETMASK_SIZE 6
#define ABBREV_BITS_INET4_SUBNET 25
#endif

static int32 network_cmp_internal(inet *a1, inet *a2);
static bool addressOK(unsigned char *a, int bits, int family);
static inet *internal_inetpl(inet *ip, int64 addend);

/* sortsupport for inet/cidr */
typedef struct
{
	int64		input_count;	/* number of non-null values seen */
	bool		estimating;		/* true if estimating cardinality */

	hyperLogLogState abbr_card; /* cardinality estimator */
}			network_sortsupport_state;

static int	network_cmp_internal(inet *a1, inet *a2);
static int	network_fast_cmp(Datum x, Datum y, SortSupport ssup);
static int	network_cmp_abbrev(Datum x, Datum y, SortSupport ssup);
static bool network_abbrev_abort(int memtupcount, SortSupport ssup);
static Datum network_abbrev_convert(Datum original, SortSupport ssup);

/*
 * Common INET/CIDR input routine
 */
static inet *
network_in(char *src, bool is_cidr)
{
	int			bits;
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	/*
	 * First, check to see if this is an IPv6 or IPv4 address.  IPv6 addresses
	 * will have a : somewhere in them (several, in fact) so if there is one
	 * present, assume it's V6, otherwise assume it's V4.
	 */

	if (strchr(src, ':') != NULL)
		ip_family(dst) = PGSQL_AF_INET6;
	else
		ip_family(dst) = PGSQL_AF_INET;

	bits = inet_net_pton(ip_family(dst), src, ip_addr(dst),
						 is_cidr ? ip_addrsize(dst) : -1);
	if ((bits < 0) || (bits > ip_maxbits(dst)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
		/* translator: first %s is inet or cidr */
				 errmsg("invalid input syntax for type %s: \"%s\"",
						is_cidr ? "cidr" : "inet", src)));

	/*
	 * Error check: CIDR values must not have any bits set beyond the masklen.
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(dst), bits, ip_family(dst)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid cidr value: \"%s\"", src),
					 errdetail("Value has bits set to right of mask.")));
	}

	ip_bits(dst) = bits;
	SET_INET_VARSIZE(dst);

	return dst;
}

Datum
inet_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, false));
}

Datum
cidr_in(PG_FUNCTION_ARGS)
{
	char	   *src = PG_GETARG_CSTRING(0);

	PG_RETURN_INET_P(network_in(src, true));
}


/*
 * Common INET/CIDR output routine
 */
static char *
network_out(inet *src, bool is_cidr)
{
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];
	char	   *dst;
	int			len;

	dst = inet_net_ntop(ip_family(src), ip_addr(src), ip_bits(src),
						tmp, sizeof(tmp));
	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* For CIDR, add /n if not present */
	if (is_cidr && strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(src));
	}

	return pstrdup(tmp);
}

Datum
inet_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, false));
}

Datum
cidr_out(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);

	PG_RETURN_CSTRING(network_out(src, true));
}


/*
 *		network_recv		- converts external binary format to inet
 *
 * The external representation is (one byte apiece for)
 * family, bits, is_cidr, address length, address in network byte order.
 *
 * Presence of is_cidr is largely for historical reasons, though it might
 * allow some code-sharing on the client side.  We send it correctly on
 * output, but ignore the value on input.
 */
static inet *
network_recv(StringInfo buf, bool is_cidr)
{
	inet	   *addr;
	char	   *addrptr;
	int			bits;
	int			nb,
				i;

	/* make sure any unused bits in a CIDR value are zeroed */
	addr = (inet *) palloc0(sizeof(inet));

	ip_family(addr) = pq_getmsgbyte(buf);
	if (ip_family(addr) != PGSQL_AF_INET &&
		ip_family(addr) != PGSQL_AF_INET6)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid address family in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	bits = pq_getmsgbyte(buf);
	if (bits < 0 || bits > ip_maxbits(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid bits in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));
	ip_bits(addr) = bits;
	i = pq_getmsgbyte(buf);		/* ignore is_cidr */
	nb = pq_getmsgbyte(buf);
	if (nb != ip_addrsize(addr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
		/* translator: %s is inet or cidr */
				 errmsg("invalid length in external \"%s\" value",
						is_cidr ? "cidr" : "inet")));

	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		addrptr[i] = pq_getmsgbyte(buf);

	/*
	 * Error check: CIDR values must not have any bits set beyond the masklen.
	 */
	if (is_cidr)
	{
		if (!addressOK(ip_addr(addr), bits, ip_family(addr)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
					 errmsg("invalid external \"cidr\" value"),
					 errdetail("Value has bits set to right of mask.")));
	}

	SET_INET_VARSIZE(addr);

	return addr;
}

Datum
inet_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, false));
}

Datum
cidr_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);

	PG_RETURN_INET_P(network_recv(buf, true));
}


/*
 *		network_send		- converts inet to binary format
 */
static bytea *
network_send(inet *addr, bool is_cidr)
{
	StringInfoData buf;
	char	   *addrptr;
	int			nb,
				i;

	pq_begintypsend(&buf);
	pq_sendbyte(&buf, ip_family(addr));
	pq_sendbyte(&buf, ip_bits(addr));
	pq_sendbyte(&buf, is_cidr);
	nb = ip_addrsize(addr);
	if (nb < 0)
		nb = 0;
	pq_sendbyte(&buf, nb);
	addrptr = (char *) ip_addr(addr);
	for (i = 0; i < nb; i++)
		pq_sendbyte(&buf, addrptr[i]);
	return pq_endtypsend(&buf);
}

Datum
inet_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, false));
}

Datum
cidr_send(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);

	PG_RETURN_BYTEA_P(network_send(addr, true));
}


Datum
inet_to_cidr(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits;

	bits = ip_bits(src);

	/* safety check */
	if ((bits < 0) || (bits > ip_maxbits(src)))
		elog(ERROR, "invalid inet bit length: %d", bits);

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

Datum
inet_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);
	inet	   *dst;

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	/* clone the original data */
	dst = (inet *) palloc(VARSIZE_ANY(src));
	memcpy(dst, src, VARSIZE_ANY(src));

	ip_bits(dst) = bits;

	PG_RETURN_INET_P(dst);
}

Datum
cidr_set_masklen(PG_FUNCTION_ARGS)
{
	inet	   *src = PG_GETARG_INET_PP(0);
	int			bits = PG_GETARG_INT32(1);

	if (bits == -1)
		bits = ip_maxbits(src);

	if ((bits < 0) || (bits > ip_maxbits(src)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid mask length: %d", bits)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(src, bits));
}

/*
 * Copy src and set mask length to 'bits' (which must be valid for the family)
 */
inet *
cidr_set_masklen_internal(const inet *src, int bits)
{
	inet	   *dst = (inet *) palloc0(sizeof(inet));

	ip_family(dst) = ip_family(src);
	ip_bits(dst) = bits;

	if (bits > 0)
	{
		Assert(bits <= ip_maxbits(dst));

		/* Clone appropriate bytes of the address, leaving the rest 0 */
		memcpy(ip_addr(dst), ip_addr(src), (bits + 7) / 8);

		/* Clear any unwanted bits in the last partial byte */
		if (bits % 8)
			ip_addr(dst)[bits / 8] &= ~(0xFF >> (bits % 8));
	}

	/* Set varlena header correctly */
	SET_INET_VARSIZE(dst);

	return dst;
}

/*
 *	Basic comparison function for sorting and inet/cidr comparisons.
 *
 * Comparison is first on the common bits of the network part, then on
 * the length of the network part, and then on the whole unmasked address.
 * The effect is that the network part is the major sort key, and for
 * equal network parts we sort on the host part.  Note this is only sane
 * for CIDR if address bits to the right of the mask are guaranteed zero;
 * otherwise logically-equal CIDRs might compare different.
 */

static int32
network_cmp_internal(inet *a1, inet *a2)
{
	if (ip_family(a1) == ip_family(a2))
	{
		int			order;

		order = bitncmp(ip_addr(a1), ip_addr(a2),
						Min(ip_bits(a1), ip_bits(a2)));
		if (order != 0)
			return order;
		order = ((int) ip_bits(a1)) - ((int) ip_bits(a2));
		if (order != 0)
			return order;
		return bitncmp(ip_addr(a1), ip_addr(a2), ip_maxbits(a1));
	}

	return ip_family(a1) - ip_family(a2);
}

Datum
network_cmp(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_INT32(network_cmp_internal(a1, a2));
}

/*
 *	SortSupport implementation functions.
 */

/*
 * SortSupport strategy function. Populates a SortSupport struct with the
 * information necessary to use comparison by abbreviated keys.
 */
Datum
network_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = network_fast_cmp;
	ssup->ssup_extra = NULL;

	if (ssup->abbreviate)
	{
		network_sortsupport_state *uss;
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(ssup->ssup_cxt);

		uss = palloc(sizeof(network_sortsupport_state));
		uss->input_count = 0;
		uss->estimating = true;
		initHyperLogLog(&uss->abbr_card, 10);

		ssup->ssup_extra = uss;

		ssup->comparator = network_cmp_abbrev;
		ssup->abbrev_converter = network_abbrev_convert;
		ssup->abbrev_abort = network_abbrev_abort;
		ssup->abbrev_full_comparator = network_fast_cmp;

		MemoryContextSwitchTo(oldcontext);
	}

	PG_RETURN_VOID();
}

/*
 * SortSupport authoritative comparison function. Pulls two inet structs from
 * the heap and runs a standard comparison on them.
 */
static int
network_fast_cmp(Datum x, Datum y, SortSupport ssup)
{
	inet	   *arg1 = DatumGetInetPP(x);
	inet	   *arg2 = DatumGetInetPP(y);

	return network_cmp_internal(arg1, arg2);
}

/*
 * SortSupport abbreviated key comparison function. Compares two inet/cidr
 * values addresses quickly by treating them like integers, and without having
 * to go to the heap. This works because we've packed them in a form that can
 * support this comparison in network_abbrev_convert.
 */
static int
network_cmp_abbrev(Datum x, Datum y, SortSupport ssup)
{
	if (x > y)
		return 1;
	else if (x == y)
		return 0;
	else
		return -1;
}

/*
 * Callback for estimating effectiveness of abbreviated key optimization.
 *
 * We pay no attention to the cardinality of the non-abbreviated data, because
 * there is no equality fast-path within authoritative inet comparator.
 */
static bool
network_abbrev_abort(int memtupcount, SortSupport ssup)
{
	network_sortsupport_state *uss = ssup->ssup_extra;
	double		abbr_card;

	if (memtupcount < 10000 || uss->input_count < 10000 || !uss->estimating)
		return false;

	abbr_card = estimateHyperLogLog(&uss->abbr_card);

	/*
	 * If we have >100k distinct values, then even if we were sorting many
	 * billion rows we'd likely still break even, and the penalty of undoing
	 * that many rows of abbrevs would probably not be worth it. At this point
	 * we stop counting because we know that we're now fully committed.
	 */
	if (abbr_card > 100000.0)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "network_abbrev: estimation ends at cardinality %f"
				 " after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count, memtupcount);
#endif
		uss->estimating = false;
		return false;
	}

	/*
	 * Target minimum cardinality is 1 per ~2k of non-null inputs. 0.5 row
	 * fudge factor allows us to abort earlier on genuinely pathological data
	 * where we've had exactly one abbreviated value in the first 2k
	 * (non-null) rows.
	 */
	if (abbr_card < uss->input_count / 2000.0 + 0.5)
	{
#ifdef TRACE_SORT
		if (trace_sort)
			elog(LOG,
				 "network_abbrev: aborting abbreviation at cardinality %f"
				 " below threshold %f after " INT64_FORMAT " values (%d rows)",
				 abbr_card, uss->input_count / 2000.0 + 0.5, uss->input_count,
				 memtupcount);
#endif
		return true;
	}

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "network_abbrev: cardinality %f after " INT64_FORMAT
			 " values (%d rows)", abbr_card, uss->input_count, memtupcount);
#endif

	return false;
}

/*
 * SortSupport conversion routine. Converts original inet/cidr representations
 * to abbreviated keys . The inet/cidr types are pass-by-reference, so is an
 * optimization so that sorting routines don't have to pull full values from
 * the heap to compare.
 *
 * All inet values have three major components (and take for example the
 * address 1.2.3.4/24):
 *
 *     * Their netmasked bits (1.2.3.0).
 *     * Their netmask size (/24).
 *     * Their subnet bits (0.0.0.4).
 *
 * cidr values are the same except that with only the first two components --
 * all their subnet bits *must* be zero (1.2.3.0/24).
 *
 * IPv4 and IPv6 are identical in this makeup, with the difference being that
 * IPv4 addresses have a maximum of 32 bits compared to IPv6's 64 bits, so in
 * IPv6 each part may be larger.
 *
 * inet/cdir types compare using these sorting rules. If any rule is a tie, the
 * algorithm drops through to the next to break it:
 *
 *     1. IPv4 always appears before IPv6.
 *     2. Netmasked bits are compared.
 *     3. Netmask size is compared.
 *     4. All bits are compared (but by this point we know that the netmasked
 *        bits are equal, so we're in effect only comparing subnet bits).
 *
 * When generating abbreviated keys for SortSupport, we pack in as much
 * information as we can while ensuring that when comparing those keys as
 * integers, the rules above will be respected.
 *
 * In most cases, that means just the most significant of the netmasked bits
 * are retained, but in the case of IPv4 addresses on a 64-bit machine, we can
 * hold almost all relevant information for comparisons to the degree that we
 * almost never have to fall back to authoritative comparison. In all cases,
 * there will be enough data present for the abbreviated keys to perform very
 * well in all except the very worst of circumstances (and in those, key
 * abbreviation will probably be aborted as we detect low cardinality).
 *
 * IPv4
 * ----
 *
 * 32-bit machine:
 *
 * Start with 1 bit for the IP family (IPv4 or IPv6; this bit is present in
 * every case below) followed by all but 1 of the netmasked bits. If those are
 * all equal, the system will have to fall back to non-abbreviated comparison.
 *
 * +----------+---------------------+
 * | 1 bit IP |   31 bits netmask   |     (up to 1 bit
 * |  family  |     (truncated)     |   netmask omitted)
 * +----------+---------------------+
 *
 * 64-bit machine:
 *
 * The most interesting case: we have space to store all netmasked bits,
 * followed by the netmask size, followed by 25 bits of the subnet. We lay the
 * bits out carefully this way so that the entire value can be compared as an
 * integer while still adhering to the inet/cidr sorting rules stated above.
 *
 * 25 bits is more than enough to store most subnets (all but /6 or smaller),
 * so in the vast majority of cases these keys should hold enough information
 * to save trips to the heap.
 *
 * +----------+-----------------------+--------------+--------------------+
 * | 1 bit IP |    32 bits netmask    |    6 bits    |   25 bits subnet   |
 * |  family  |        (full)         | netmask size |    (truncated)     |
 * +----------+-----------------------+--------------+--------------------+
 *
 * IPv6
 * ----
 *
 * 32-bit machine:
 *
 * +----------+---------------------+
 * | 1 bit IP |   31 bits netmask   |    (up to 97 bits
 * |  family  |     (truncated)     |   netmask omitted)
 * +----------+---------------------+
 *
 * 64-bit machine:
 *
 * +----------+---------------------------------+
 * | 1 bit IP |         63 bits netmask         |    (up to 65 bits
 * |  family  |           (truncated)           |   netmask omitted)
 * +----------+---------------------------------+
 *
 */
static Datum
network_abbrev_convert(Datum original, SortSupport ssup)
{
	network_sortsupport_state *uss = ssup->ssup_extra;
	inet	   *authoritative = DatumGetInetPP(original);
	Datum		res;
	Datum		ipaddr_int,
				netmask_int,
				subnet_int;
	char		datum_size_left,
				datum_subnet_size;

	/* If another IP family is ever added, we'll need to redesign the key
	 * abbreviation strategy. */
	Assert(ip_family(authoritative) == PGSQL_AF_INET ||
		   ip_family(authoritative) == PGSQL_AF_INET6);

	res = (Datum) 0;
	if (ip_family(authoritative) == PGSQL_AF_INET6)
	{
		/* Shift a 1 over to the datum's most significant bit. */
		res = ((Datum) 1) << SIZEOF_DATUM * BITS_PER_BYTE - ABBREV_BITS_INET_FAMILY;
	}

	/*
	 * For IPv6, create an integer from either the first 4 or 8 bytes of the IP
	 * (depending on whether we're on a 32 or 64-bit machine). For IPv4, always
	 * use all 4 bytes.
	 *
	 * We're consuming an array of char, so make sure to byteswap on little
	 * endian systems (an inet's IP array emulates big endian in that the
	 * first byte is always the most significant).
	 */
	if (ip_family(authoritative) == PGSQL_AF_INET6)
	{
		ipaddr_int = *((Datum *) ip_addr(authoritative));
		ipaddr_int = DatumBigEndianToNative(ipaddr_int);
	}
	else
	{
		uint32		ipaddr_int32 = *((uint32 *) ip_addr(authoritative));
#ifndef WORDS_BIGENDIAN
		ipaddr_int = pg_bswap32(ipaddr_int32);
#endif
	}

	netmask_int = ipaddr_int;
	subnet_int = (Datum) 0;
	datum_subnet_size = 0;

	/* Extract subnet bits from ipaddr_ints if there are some present. */
	datum_size_left = SIZEOF_DATUM * BITS_PER_BYTE - ip_bits(authoritative);

	/*
	 * Number of bits in subnet. e.g. An IPv4 that's /24 is 32 - 24 = 8.
	 *
	 * However, only some of the bits may have made it into the fixed sized
	 * datum, so take the smallest number between bits in the subnet and bits
	 * left in the datum.
	 */
	datum_subnet_size = Min(ip_maxbits(authoritative) - ip_bits(authoritative),
							datum_size_left);

	if (datum_subnet_size > 0)
	{
		/*
		 * This shift creates a power of two like `0010 0000`, and subtracts
		 * one to create a bitmask for an IP's subnet bits like `0001 1111`.
		 */
		Assert(datum_subnet_size < SIZEOF_DATUM * BITS_PER_BYTE);
		Datum		subnet_bitmask = (((Datum) 1) << datum_subnet_size) - 1;

		/*
		 * AND the bitmask with the IP address' integer to truncate it down to
		 * just the bits after the netmask.
		 */
		subnet_int = ipaddr_int & subnet_bitmask;

		/*
		 * Integer representation for the netmask is the IP's integer minus
		 * whatever we calculated for the subnet.
		 */
		netmask_int = ipaddr_int - subnet_int;
	}

#if SIZEOF_DATUM == 8

	if (ip_family(authoritative) == PGSQL_AF_INET6)
	{
		/*
		 * IPv6 on a 64-bit machine: keep the most significant 63 netmasked
		 * bits.
		 */
		res |= netmask_int >> ABBREV_BITS_INET_FAMILY;
	}
	else
	{
		/*
		 * IPv4 on a 64-bit machine: keep all 32 netmasked bits, netmask size,
		 * and most significant 25 subnet bits (see diagram above for more
		 * detail).
		 */

		Datum		netmask_shifted;
		Datum		netmask_size_and_subnet = 0;

		/*
		 * an IPv4 netmask has a maximum value of 32 which takes 6 bits to
		 * contain
		 */
		Datum		netmask_size = (Datum) ip_bits(authoritative);

		Assert(netmask_size <= ip_maxbits(authoritative));
		netmask_size_and_subnet |= netmask_size << ABBREV_BITS_INET4_SUBNET;

		/*
		 * if we have more than 25 subnet bits of information, shift it down
		 * to the available size
		 */
		if (datum_subnet_size > ABBREV_BITS_INET4_SUBNET)
		{
			subnet_int >>= datum_subnet_size - ABBREV_BITS_INET4_SUBNET;
		}
		netmask_size_and_subnet |= subnet_int;

		/*
		 * There's a fair bit of scary bit manipulation in this function. This
		 * is an extra check that verifies that that nothing outside of the
		 * least signifiant 31 bits is set.
		 *
		 * (PG_UINT32_MAX >> 1) = 31 bits of 1s
		 */
		Assert((netmask_size_and_subnet | (PG_UINT32_MAX >> 1)) == (PG_UINT32_MAX >> 1));

		/*
		 * Shift left 31 bits: 6 bits netmask size + 25 subnet bits.
		 *
		 * And assert the opposite as above: no bits should be set in the least
		 * significant 31 positions where we store netmask size and subnet.
		 */
		netmask_shifted = netmask_int << (ABBREV_BITS_INET4_NETMASK_SIZE + ABBREV_BITS_INET4_SUBNET);
		Assert((netmask_shifted & ~(PG_UINT32_MAX >> 1)) == netmask_shifted);

		res |= netmask_shifted | netmask_size_and_subnet;
	}

#else							/* SIZEOF_DATUM != 8 */

	/*
	 * 32-bit machine: keep the most significant 31 netmasked bits in both IPv4
	 * and IPv6.
	 */
	res |= netmask_int >> ABBREV_BITS_INET_FAMILY;

#endif

	uss->input_count += 1;

	/*
	 * Cardinality estimation. The estimate uses uint32, so on a 64-bit
	 * machine, XOR the two 32-bit halves together to produce slightly more
	 * entropy.
	 */
	if (uss->estimating)
	{
		uint32		tmp;

#if SIZEOF_DATUM == 8
		tmp = (uint32) res ^ (uint32) ((uint64) res >> 32);
#else							/* SIZEOF_DATUM != 8 */
		tmp = (uint32) res;
#endif

		addHyperLogLog(&uss->abbr_card, DatumGetUInt32(hash_uint32(tmp)));
	}

	return res;
}

/*
 *	Boolean ordering tests.
 */
Datum
network_lt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) < 0);
}

Datum
network_le(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) <= 0);
}

Datum
network_eq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) == 0);
}

Datum
network_ge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) >= 0);
}

Datum
network_gt(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) > 0);
}

Datum
network_ne(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(network_cmp_internal(a1, a2) != 0);
}

/*
 * MIN/MAX support functions.
 */
Datum
network_smaller(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) < 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

Datum
network_larger(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (network_cmp_internal(a1, a2) > 0)
		PG_RETURN_INET_P(a1);
	else
		PG_RETURN_INET_P(a2);
}

/*
 * Support function for hash indexes on inet/cidr.
 */
Datum
hashinet(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	/* XXX this assumes there are no pad bytes in the data structure */
	return hash_any((unsigned char *) VARDATA_ANY(addr), addrsize + 2);
}

Datum
hashinetextended(PG_FUNCTION_ARGS)
{
	inet	   *addr = PG_GETARG_INET_PP(0);
	int			addrsize = ip_addrsize(addr);

	return hash_any_extended((unsigned char *) VARDATA_ANY(addr), addrsize + 2,
							 PG_GETARG_INT64(1));
}

/*
 *	Boolean network-inclusion tests.
 */
Datum
network_sub(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) > ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_subeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) >= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a2)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_sup(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) < ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_supeq(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(ip_bits(a1) <= ip_bits(a2) &&
					   bitncmp(ip_addr(a1), ip_addr(a2), ip_bits(a1)) == 0);
	}

	PG_RETURN_BOOL(false);
}

Datum
network_overlap(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	if (ip_family(a1) == ip_family(a2))
	{
		PG_RETURN_BOOL(bitncmp(ip_addr(a1), ip_addr(a2),
							   Min(ip_bits(a1), ip_bits(a2))) == 0);
	}

	PG_RETURN_BOOL(false);
}

/*
 * Extract data from a network datatype.
 */
Datum
network_host(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *ptr;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	/* force display of max bits, regardless of masklen... */
	if (inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
					  tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* Suppress /n if present (shouldn't happen now) */
	if ((ptr = strchr(tmp, '/')) != NULL)
		*ptr = '\0';

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

/*
 * network_show implements the inet and cidr casts to text.  This is not
 * quite the same behavior as network_out, hence we can't drop it in favor
 * of CoerceViaIO.
 */
Datum
network_show(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int			len;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	if (inet_net_ntop(ip_family(ip), ip_addr(ip), ip_maxbits(ip),
					  tmp, sizeof(tmp)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	/* Add /n if not present (which it won't be) */
	if (strchr(tmp, '/') == NULL)
	{
		len = strlen(tmp);
		snprintf(tmp + len, sizeof(tmp) - len, "/%u", ip_bits(ip));
	}

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
inet_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = inet_net_ntop(ip_family(ip), ip_addr(ip),
						ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format inet value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
cidr_abbrev(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	char	   *dst;
	char		tmp[sizeof("xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:255.255.255.255/128")];

	dst = inet_cidr_ntop(ip_family(ip), ip_addr(ip),
						 ip_bits(ip), tmp, sizeof(tmp));

	if (dst == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
				 errmsg("could not format cidr value: %m")));

	PG_RETURN_TEXT_P(cstring_to_text(tmp));
}

Datum
network_masklen(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	PG_RETURN_INT32(ip_bits(ip));
}

Datum
network_family(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);

	switch (ip_family(ip))
	{
		case PGSQL_AF_INET:
			PG_RETURN_INT32(4);
			break;
		case PGSQL_AF_INET6:
			PG_RETURN_INT32(6);
			break;
		default:
			PG_RETURN_INT32(0);
			break;
	}
}

Datum
network_broadcast(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	for (byte = 0; byte < maxbytes; byte++)
	{
		if (bits >= 8)
		{
			mask = 0x00;
			bits -= 8;
		}
		else if (bits == 0)
			mask = 0xff;
		else
		{
			mask = 0xff >> bits;
			bits = 0;
		}

		b[byte] = a[byte] | mask;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_network(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *a,
			   *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	a = ip_addr(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = a[byte] & mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_bits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_netmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	unsigned char mask;
	unsigned char *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	bits = ip_bits(ip);
	b = ip_addr(dst);

	byte = 0;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff << (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte++;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

Datum
network_hostmask(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;
	int			byte;
	int			bits;
	int			maxbytes;
	unsigned char mask;
	unsigned char *b;

	/* make sure any unused bits are zeroed */
	dst = (inet *) palloc0(sizeof(inet));

	maxbytes = ip_addrsize(ip);
	bits = ip_maxbits(ip) - ip_bits(ip);
	b = ip_addr(dst);

	byte = maxbytes - 1;

	while (bits)
	{
		if (bits >= 8)
		{
			mask = 0xff;
			bits -= 8;
		}
		else
		{
			mask = 0xff >> (8 - bits);
			bits = 0;
		}

		b[byte] = mask;
		byte--;
	}

	ip_family(dst) = ip_family(ip);
	ip_bits(dst) = ip_maxbits(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}

/*
 * Returns true if the addresses are from the same family, or false.  Used to
 * check that we can create a network which contains both of the networks.
 */
Datum
inet_same_family(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0);
	inet	   *a2 = PG_GETARG_INET_PP(1);

	PG_RETURN_BOOL(ip_family(a1) == ip_family(a2));
}

/*
 * Returns the smallest CIDR which contains both of the inputs.
 */
Datum
inet_merge(PG_FUNCTION_ARGS)
{
	inet	   *a1 = PG_GETARG_INET_PP(0),
			   *a2 = PG_GETARG_INET_PP(1);
	int			commonbits;

	if (ip_family(a1) != ip_family(a2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot merge addresses from different families")));

	commonbits = bitncommon(ip_addr(a1), ip_addr(a2),
							Min(ip_bits(a1), ip_bits(a2)));

	PG_RETURN_INET_P(cidr_set_masklen_internal(a1, commonbits));
}

/*
 * Convert a value of a network datatype to an approximate scalar value.
 * This is used for estimating selectivities of inequality operators
 * involving network types.
 *
 * On failure (e.g., unsupported typid), set *failure to true;
 * otherwise, that variable is not changed.
 */
double
convert_network_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case INETOID:
		case CIDROID:
			{
				inet	   *ip = DatumGetInetPP(value);
				int			len;
				double		res;
				int			i;

				/*
				 * Note that we don't use the full address for IPv6.
				 */
				if (ip_family(ip) == PGSQL_AF_INET)
					len = 4;
				else
					len = 5;

				res = ip_family(ip);
				for (i = 0; i < len; i++)
				{
					res *= 256;
					res += ip_addr(ip)[i];
				}
				return res;
			}
		case MACADDROID:
			{
				macaddr    *mac = DatumGetMacaddrP(value);
				double		res;

				res = (mac->a << 16) | (mac->b << 8) | (mac->c);
				res *= 256 * 256 * 256;
				res += (mac->d << 16) | (mac->e << 8) | (mac->f);
				return res;
			}
		case MACADDR8OID:
			{
				macaddr8   *mac = DatumGetMacaddr8P(value);
				double		res;

				res = (mac->a << 24) | (mac->b << 16) | (mac->c << 8) | (mac->d);
				res *= ((double) 256) * 256 * 256 * 256;
				res += (mac->e << 24) | (mac->f << 16) | (mac->g << 8) | (mac->h);
				return res;
			}
	}

	*failure = true;
	return 0;
}

/*
 * int
 * bitncmp(l, r, n)
 *		compare bit masks l and r, for n bits.
 * return:
 *		<0, >0, or 0 in the libc tradition.
 * note:
 *		network byte order assumed.  this means 192.5.5.240/28 has
 *		0x11110000 in its fourth octet.
 * author:
 *		Paul Vixie (ISC), June 1996
 */
int
bitncmp(const unsigned char *l, const unsigned char *r, int n)
{
	unsigned int lb,
				rb;
	int			x,
				b;

	b = n / 8;
	x = memcmp(l, r, b);
	if (x || (n % 8) == 0)
		return x;

	lb = l[b];
	rb = r[b];
	for (b = n % 8; b > 0; b--)
	{
		if (IS_HIGHBIT_SET(lb) != IS_HIGHBIT_SET(rb))
		{
			if (IS_HIGHBIT_SET(lb))
				return 1;
			return -1;
		}
		lb <<= 1;
		rb <<= 1;
	}
	return 0;
}

/*
 * bitncommon: compare bit masks l and r, for up to n bits.
 *
 * Returns the number of leading bits that match (0 to n).
 */
int
bitncommon(const unsigned char *l, const unsigned char *r, int n)
{
	int			byte,
				nbits;

	/* number of bits to examine in last byte */
	nbits = n % 8;

	/* check whole bytes */
	for (byte = 0; byte < n / 8; byte++)
	{
		if (l[byte] != r[byte])
		{
			/* at least one bit in the last byte is not common */
			nbits = 7;
			break;
		}
	}

	/* check bits in last partial byte */
	if (nbits != 0)
	{
		/* calculate diff of first non-matching bytes */
		unsigned int diff = l[byte] ^ r[byte];

		/* compare the bits from the most to the least */
		while ((diff >> (8 - nbits)) != 0)
			nbits--;
	}

	return (8 * byte) + nbits;
}


/*
 * Verify a CIDR address is OK (doesn't have bits set past the masklen)
 */
static bool
addressOK(unsigned char *a, int bits, int family)
{
	int			byte;
	int			nbits;
	int			maxbits;
	int			maxbytes;
	unsigned char mask;

	if (family == PGSQL_AF_INET)
	{
		maxbits = 32;
		maxbytes = 4;
	}
	else
	{
		maxbits = 128;
		maxbytes = 16;
	}
	Assert(bits <= maxbits);

	if (bits == maxbits)
		return true;

	byte = bits / 8;

	nbits = bits % 8;
	mask = 0xff;
	if (bits != 0)
		mask >>= nbits;

	while (byte < maxbytes)
	{
		if ((a[byte] & mask) != 0)
			return false;
		mask = 0xff;
		byte++;
	}

	return true;
}


/*
 * These functions are used by planner to generate indexscan limits
 * for clauses a << b and a <<= b
 */

/* return the minimal value for an IP on a given network */
Datum
network_scan_first(Datum in)
{
	return DirectFunctionCall1(network_network, in);
}

/*
 * return "last" IP on a given network. It's the broadcast address,
 * however, masklen has to be set to its max bits, since
 * 192.168.0.255/24 is considered less than 192.168.0.255/32
 *
 * inet_set_masklen() hacked to max out the masklength to 128 for IPv6
 * and 32 for IPv4 when given '-1' as argument.
 */
Datum
network_scan_last(Datum in)
{
	return DirectFunctionCall2(inet_set_masklen,
							   DirectFunctionCall1(network_broadcast, in),
							   Int32GetDatum(-1));
}


/*
 * IP address that the client is connecting from (NULL if Unix socket)
 */
Datum
inet_client_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 remote_host, sizeof(remote_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->raddr.addr.ss_family, remote_host);

	PG_RETURN_INET_P(network_in(remote_host, false));
}


/*
 * port that the client is connecting from (NULL if Unix socket)
 */
Datum
inet_client_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		remote_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->raddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	remote_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->raddr.addr, port->raddr.salen,
							 NULL, 0,
							 remote_port, sizeof(remote_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(remote_port)));
}


/*
 * IP address that the server accepted the connection on (NULL if Unix socket)
 */
Datum
inet_server_addr(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_host[NI_MAXHOST];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	local_host[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 local_host, sizeof(local_host),
							 NULL, 0,
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	clean_ipv6_addr(port->laddr.addr.ss_family, local_host);

	PG_RETURN_INET_P(network_in(local_host, false));
}


/*
 * port that the server accepted the connection on (NULL if Unix socket)
 */
Datum
inet_server_port(PG_FUNCTION_ARGS)
{
	Port	   *port = MyProcPort;
	char		local_port[NI_MAXSERV];
	int			ret;

	if (port == NULL)
		PG_RETURN_NULL();

	switch (port->laddr.addr.ss_family)
	{
		case AF_INET:
#ifdef HAVE_IPV6
		case AF_INET6:
#endif
			break;
		default:
			PG_RETURN_NULL();
	}

	local_port[0] = '\0';

	ret = pg_getnameinfo_all(&port->laddr.addr, port->laddr.salen,
							 NULL, 0,
							 local_port, sizeof(local_port),
							 NI_NUMERICHOST | NI_NUMERICSERV);
	if (ret != 0)
		PG_RETURN_NULL();

	PG_RETURN_DATUM(DirectFunctionCall1(int4in, CStringGetDatum(local_port)));
}


Datum
inetnot(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);

		while (nb-- > 0)
			pdst[nb] = ~pip[nb];
	}
	ip_bits(dst) = ip_bits(ip);

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetand(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot AND inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (nb-- > 0)
			pdst[nb] = pip[nb] & pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


Datum
inetor(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot OR inet values of different sizes")));
	else
	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		unsigned char *pdst = ip_addr(dst);

		while (nb-- > 0)
			pdst[nb] = pip[nb] | pip2[nb];
	}
	ip_bits(dst) = Max(ip_bits(ip), ip_bits(ip2));

	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	PG_RETURN_INET_P(dst);
}


static inet *
internal_inetpl(inet *ip, int64 addend)
{
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	{
		int			nb = ip_addrsize(ip);
		unsigned char *pip = ip_addr(ip);
		unsigned char *pdst = ip_addr(dst);
		int			carry = 0;

		while (nb-- > 0)
		{
			carry = pip[nb] + (int) (addend & 0xFF) + carry;
			pdst[nb] = (unsigned char) (carry & 0xFF);
			carry >>= 8;

			/*
			 * We have to be careful about right-shifting addend because
			 * right-shift isn't portable for negative values, while simply
			 * dividing by 256 doesn't work (the standard rounding is in the
			 * wrong direction, besides which there may be machines out there
			 * that round the wrong way).  So, explicitly clear the low-order
			 * byte to remove any doubt about the correct result of the
			 * division, and then divide rather than shift.
			 */
			addend &= ~((int64) 0xFF);
			addend /= 0x100;
		}

		/*
		 * At this point we should have addend and carry both zero if original
		 * addend was >= 0, or addend -1 and carry 1 if original addend was <
		 * 0.  Anything else means overflow.
		 */
		if (!((addend == 0 && carry == 0) ||
			  (addend == -1 && carry == 1)))
			ereport(ERROR,
					(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
					 errmsg("result is out of range")));
	}

	ip_bits(dst) = ip_bits(ip);
	ip_family(dst) = ip_family(ip);
	SET_INET_VARSIZE(dst);

	return dst;
}


Datum
inetpl(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, addend));
}


Datum
inetmi_int8(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	int64		addend = PG_GETARG_INT64(1);

	PG_RETURN_INET_P(internal_inetpl(ip, -addend));
}


Datum
inetmi(PG_FUNCTION_ARGS)
{
	inet	   *ip = PG_GETARG_INET_PP(0);
	inet	   *ip2 = PG_GETARG_INET_PP(1);
	int64		res = 0;

	if (ip_family(ip) != ip_family(ip2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cannot subtract inet values of different sizes")));
	else
	{
		/*
		 * We form the difference using the traditional complement, increment,
		 * and add rule, with the increment part being handled by starting the
		 * carry off at 1.  If you don't think integer arithmetic is done in
		 * two's complement, too bad.
		 */
		int			nb = ip_addrsize(ip);
		int			byte = 0;
		unsigned char *pip = ip_addr(ip);
		unsigned char *pip2 = ip_addr(ip2);
		int			carry = 1;

		while (nb-- > 0)
		{
			int			lobyte;

			carry = pip[nb] + (~pip2[nb] & 0xFF) + carry;
			lobyte = carry & 0xFF;
			if (byte < sizeof(int64))
			{
				res |= ((int64) lobyte) << (byte * 8);
			}
			else
			{
				/*
				 * Input wider than int64: check for overflow.  All bytes to
				 * the left of what will fit should be 0 or 0xFF, depending on
				 * sign of the now-complete result.
				 */
				if ((res < 0) ? (lobyte != 0xFF) : (lobyte != 0))
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("result is out of range")));
			}
			carry >>= 8;
			byte++;
		}

		/*
		 * If input is narrower than int64, overflow is not possible, but we
		 * have to do proper sign extension.
		 */
		if (carry == 0 && byte < sizeof(int64))
			res |= ((uint64) (int64) -1) << (byte * 8);
	}

	PG_RETURN_INT64(res);
}


/*
 * clean_ipv6_addr --- remove any '%zone' part from an IPv6 address string
 *
 * XXX This should go away someday!
 *
 * This is a kluge needed because we don't yet support zones in stored inet
 * values.  Since the result of getnameinfo() might include a zone spec,
 * call this to remove it anywhere we want to feed getnameinfo's output to
 * network_in.  Beats failing entirely.
 *
 * An alternative approach would be to let network_in ignore %-parts for
 * itself, but that would mean we'd silently drop zone specs in user input,
 * which seems not such a good idea.
 */
void
clean_ipv6_addr(int addr_family, char *addr)
{
#ifdef HAVE_IPV6
	if (addr_family == AF_INET6)
	{
		char	   *pct = strchr(addr, '%');

		if (pct)
			*pct = '\0';
	}
#endif
}

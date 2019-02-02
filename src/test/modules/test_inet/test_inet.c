/*--------------------------------------------------------------------------
 *
 * test_inet.c
 *		Test correctness of the inet/cidr data types.
 *
 * Copyright (c) 2009-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		src/test/modules/test_inet/test_inet.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "utils/inet.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_inet_abbrev_convert);

Datum
test_inet_abbrev_convert(PG_FUNCTION_ARGS)
{
	inet *src = PG_GETARG_INET_PP(0);
	Datum res = network_abbrev_convert_var(src);
	PG_RETURN_CSTRING(psprintf("%lx", res));
}

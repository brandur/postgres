CREATE EXTENSION test_inet;

--
-- These tests return a bit-level representation of the abbreviated key for the
-- given inet value. Because a lot of fairly tricky bit manipulation is done to
-- assemble these abbreviated keys, they're here to help give us confidence
-- that we got everything right.
--
SELECT *
FROM (VALUES
	('0.0.0.0/0'::inet, '0', test_inet_abbrev_convert('0.0.0.0/0')),

	-- with 1 in the least signification position, not enough subnet bits in
	-- abbreviated key to store it, which is why the result comes out as 0
	('0.0.0.1/0'::inet, '0', test_inet_abbrev_convert('0.0.0.1/0')),

	('255.0.0.0/0'::inet, '1fe0000', test_inet_abbrev_convert('255.0.0.0/0')),
	('255.255.255.255/0'::inet, '1ffffff', test_inet_abbrev_convert('255.255.255.255/0')),
	('0.0.0.0/1'::inet, '2000000', test_inet_abbrev_convert('0.0.0.0/1')),
	('0.0.0.0/32'::inet, '40000000', test_inet_abbrev_convert('0.0.0.0/32')),
	('0.0.0.1/32'::inet, 'c0000000', test_inet_abbrev_convert('0.0.0.1/32')),
	('255.255.255.255/1'::inet, '4000000003ffffff', test_inet_abbrev_convert('255.255.255.255/1')),
	('255.255.255.255/16'::inet, '7fff80002000ffff', test_inet_abbrev_convert('255.255.255.255/16')),

	-- note all netmask bits except the one most significant bit (reserved for
	-- IP family) are 1s
	('255.255.255.255/32'::inet, '7fffffffc0000000', test_inet_abbrev_convert('255.255.255.255/32')),

	-- only the most significant bit is a 1
	('::/0'::inet, '8000000000000000', test_inet_abbrev_convert('::/0')),

	('::1/0'::inet, '8000000000000000', test_inet_abbrev_convert('::1/0')),

	-- the boundary of bits in the netmask, all of which are are too
	-- insignificant to fit into the abbreviated key
	('::1:ffff:ffff:ffff:ffff/128'::inet, '8000000000000000', test_inet_abbrev_convert('::1:ffff:ffff:ffff:ffff/128')),

	-- but add just one and we get a value in the abbreviated key
	('::2:ffff:ffff:ffff:ffff/128'::inet, '8000000000000001', test_inet_abbrev_convert('::2:ffff:ffff:ffff:ffff/128')),

	('ffff:ffff:ffff:fffd::/128'::inet, 'fffffffffffffffe', test_inet_abbrev_convert('ffff:ffff:ffff:fffd::/128')),

	-- abbreviated key representation comes out as the maximum possible value;
	-- to disambiguate between this and higher numbers, Postgres will have to
	-- fall back to authoritative comparison
	('ffff:ffff:ffff:fffe::/128'::inet, 'ffffffffffffffff', test_inet_abbrev_convert('ffff:ffff:ffff:fffe::/128')),

	('ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128'::inet, 'ffffffffffffffff', test_inet_abbrev_convert('ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff/128'))
)
AS t (original, abbrev_expected, abbrev_actual)
ORDER BY original;

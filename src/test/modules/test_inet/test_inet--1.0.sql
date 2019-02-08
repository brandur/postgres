/* src/test/modules/test_inet/test_inet--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION test_inet" to load this file. \quit

CREATE FUNCTION test_inet_abbrev_convert(original inet)
	RETURNS cstring STRICT
	AS 'MODULE_PATHNAME' LANGUAGE C;

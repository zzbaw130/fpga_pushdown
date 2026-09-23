LOAD 'MODULE_PATHNAME';

CREATE FUNCTION csd_engine_capabilities()
RETURNS text
AS 'MODULE_PATHNAME', 'csd_engine_capabilities'
LANGUAGE C STRICT;

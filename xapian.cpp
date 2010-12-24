extern "C" {
	#include <postgres.h>
	#include <fmgr.h>

	#ifdef PG_MODULE_MAGIC
	PG_MODULE_MAGIC;
	#endif
};

#include "xapian.h"


//@ text xapian_version()
extern "C" {
	PG_FUNCTION_INFO_V1(pg_xapian_version);
};
extern "C" Datum pg_xapian_version(PG_FUNCTION_ARGS)
{
	const char* r = Xapian::version_string();
	text *t = (text*) palloc(VARHDRSZ + strlen(r));
	SET_VARSIZE(t, VARHDRSZ + strlen(r));
	memcpy(VARDATA(t), r, strlen(r));
    
	PG_RETURN_TEXT_P(t);
}

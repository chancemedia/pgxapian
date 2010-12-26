extern "C" {
	#define using _using
	#define typeid _typeid
	#define typename _typename
	
	#include <postgres.h>
	#include <fmgr.h>
	#include <funcapi.h>
	
	#undef using
	#undef typeid
	#undef typename

	#ifdef PG_MODULE_MAGIC
	PG_MODULE_MAGIC;
	#endif
};

#include "xapian.h"
using namespace std;

#define XAPIAN_MAX_RESULTS 10

#define XAPIAN_CATCH_BEGIN try {
#define XAPIAN_CATCH_END \
	} \
	catch(const Xapian::Error &e) { \
		ereport(ERROR, \
			(errcode(ERRCODE_RAISE_EXCEPTION), \
				errmsg("%s", e.get_msg().c_str()), \
				errhint("context = \"%s\"", e.get_context().c_str()) \
			) \
		); \
		PG_RETURN_NULL(); \
	} \
	catch(...) { \
		ereport(ERROR, \
			(errcode(ERRCODE_RAISE_EXCEPTION), \
				errmsg("Unknown error.") \
			) \
		); \
		PG_RETURN_NULL(); \
	}


#define PG_GETARG_NTSTRING(argid) TEXT_TO_CSTRING(PG_GETARG_TEXT_P(argid))
char* TEXT_TO_CSTRING(text *t)
{
	int len = VARSIZE(t) - VARHDRSZ;
	char *r = (char*) palloc(len + 1);
	memcpy(r, VARDATA(t), len);
	r[len] = 0;
	return r;
}


//@ text xapian_version()
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_version); }
extern "C" Datum pg_xapian_version(PG_FUNCTION_ARGS)
{
	const char* r = Xapian::version_string();
	text *t = (text*) palloc(VARHDRSZ + strlen(r));
	SET_VARSIZE(t, VARHDRSZ + strlen(r));
	memcpy(VARDATA(t), r, strlen(r));
    
	PG_RETURN_TEXT_P(t);
}


//@ boolean xapian_create_index(text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_create_index); }
extern "C" Datum pg_xapian_create_index(PG_FUNCTION_ARGS)
{
	char *path = PG_GETARG_NTSTRING(0);
	
	XAPIAN_CATCH_BEGIN
		Xapian::WritableDatabase database(path, Xapian::DB_CREATE_OR_OPEN);
	XAPIAN_CATCH_END
	
	PG_RETURN_BOOL(true);
}


//@ int xapian_add_document(text, text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_add_document); }
extern "C" Datum pg_xapian_add_document(PG_FUNCTION_ARGS)
{
	char *path = PG_GETARG_NTSTRING(0);
	char *data = PG_GETARG_NTSTRING(0);
	
	XAPIAN_CATCH_BEGIN
		// open the database
		Xapian::WritableDatabase database(path, Xapian::DB_OPEN);
		
		// create the document
		Xapian::Document doc;
		doc.set_data(data);
		
		// split the data into terms
		doc.add_posting("a", 0);
		doc.add_posting("cat", 1);
		doc.add_posting("sat", 2);
		doc.add_posting("on", 3);
		doc.add_posting("a", 4);
		doc.add_posting("mat", 5);
		
		// attempt to add the document
		Xapian::docid id = database.add_document(doc);
		PG_RETURN_UINT32(id);
	XAPIAN_CATCH_END
	
	PG_RETURN_NULL();
}


//@ int xapian_count_documents(text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_count_documents); }
extern "C" Datum pg_xapian_count_documents(PG_FUNCTION_ARGS)
{
	char *path = PG_GETARG_NTSTRING(0);
	
	XAPIAN_CATCH_BEGIN
		Xapian::WritableDatabase database(path, Xapian::DB_OPEN);
		PG_RETURN_UINT32(database.get_doccount());
	XAPIAN_CATCH_END
	
	PG_RETURN_NULL();
}


typedef struct
{
	int document_id;
	int relevance;
} xapian_document;


//@ setof record xapian_match(text, text)
// create type xapian_document as ( document_id int, relevance int );
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_match); }
extern "C" Datum pg_xapian_match(PG_FUNCTION_ARGS)
{
	char *path = PG_GETARG_NTSTRING(0);
	char *terms = PG_GETARG_NTSTRING(1);
	
	XAPIAN_CATCH_BEGIN
	
	FuncCallContext *funcctx;
    TupleDesc tupdesc;
    AttInMetadata *attinmeta;

    if(SRF_IS_FIRSTCALL()) {
        funcctx = SRF_FIRSTCALL_INIT();
        
    	// open the database and do the search
    	Xapian::Database database(path);
    	Xapian::Enquire enquire(database);
    	
    	vector<string> queryterms;
    	queryterms.push_back(terms);
    	Xapian::Query query(Xapian::Query::OP_AND, queryterms.begin(), queryterms.end());
    	//elog(NOTICE, "description = %s", query.get_description().c_str());
    	
    	enquire.set_query(query);
    	Xapian::MSet matches = enquire.get_mset(0, XAPIAN_MAX_RESULTS);
    	
    	// we extract all of the results now and store them
		funcctx->user_fctx = new xapian_document[matches.size()];
		int n = 0;
		for(Xapian::MSetIterator i = matches.begin(); i != matches.end(); ++i, ++n) {
			xapian_document d = { *i, i.get_percent() };
			((xapian_document*) funcctx->user_fctx)[n] = d;
		}
    	
        MemoryContext oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        funcctx->max_calls = matches.size();

        // Build a tuple descriptor for our result type
        if(get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context "
                            "that cannot accept type record")));

        attinmeta = TupleDescGetAttInMetadata(tupdesc);
        funcctx->attinmeta = attinmeta;
        MemoryContextSwitchTo(oldcontext);
    }

    // stuff done on every call of the function
    funcctx = SRF_PERCALL_SETUP();
    attinmeta = funcctx->attinmeta;
 
    if(funcctx->call_cntr < funcctx->max_calls) {
        char **values = (char**) palloc(2 * sizeof(char *));
        values[0] = (char*) palloc(16 * sizeof(char));
        values[1] = (char*) palloc(16 * sizeof(char));
		xapian_document *matches = (xapian_document*) funcctx->user_fctx;

        snprintf(values[0], 16, "%d", matches[funcctx->call_cntr].document_id);
        snprintf(values[1], 16, "%d", matches[funcctx->call_cntr].relevance);
        HeapTuple tuple = BuildTupleFromCStrings(attinmeta, values);
        Datum result = HeapTupleGetDatum(tuple);

        SRF_RETURN_NEXT(funcctx, result);
    }
    else
    	SRF_RETURN_DONE(funcctx);
    	
	XAPIAN_CATCH_END
}

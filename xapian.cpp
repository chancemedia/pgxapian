extern "C" {
	#define using _using
	#define typeid _typeid
	#define typename _typename
	
	#include <postgres.h>
	#include <fmgr.h>
	#include <funcapi.h>
	#include <executor/spi.h>
		
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


char *xapian_get_index_path(char *name)
{
	char sql[256];
	sprintf(sql, "SELECT path FROM xapian_index WHERE name='%s'", name);
	if(SPI_execute(sql, true, 1) != SPI_OK_SELECT) {
		elog(ERROR, "\"%s\" failed", SPI_tuptable->alloced);
		return NULL;
	}
	return SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
}


//@ void xapian_drop_index(text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_drop_index); }
extern "C" Datum pg_xapian_drop_index(PG_FUNCTION_ARGS)
{
	char *name = PG_GETARG_NTSTRING(0);
	char sql[1024];
	SPI_connect();
	
	// we need the path before we remove the index
	char *path = xapian_get_index_path(name);
	
	// drop index entry
	sprintf(sql, "DELETE FROM xapian_index WHERE name='%s'", name);
	SPI_execute(sql, false, 1);
	
	// delete data files
	sprintf(sql, "rm -Rf \"%s\"", path);
	system(sql);
	
	// clean up
	SPI_finish();
	PG_RETURN_NULL();
}


int xapian_trigger_exists(char *name)
{
	char sql[256];
	sprintf(sql, "SELECT count(*) FROM pg_trigger WHERE tgname='%s'", name);
	if(SPI_execute(sql, true, 1) != SPI_OK_SELECT) {
		elog(ERROR, "SELECT count(*) FROM pg_trigger WHERE tgname='%s'", name);
		return 0;
	}
	return atoi(SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
}


int xapian_table_exists(char *table)
{
	char sql[256];
	sprintf(sql, "SELECT count(*) FROM pg_tables WHERE tablename='%s'", table);
	if(SPI_execute(sql, true, 1) != SPI_OK_SELECT) {
		elog(ERROR, "SELECT count(*) FROM pg_tables WHERE tablename='%s'", table);
		return 0;
	}
	return atoi(SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1));
}


void xapian_create_system_tables()
{
	// check if the tables already exist
	if(xapian_table_exists((char*) "xapian_index"))
		return;
	
	SPI_execute(
		"CREATE TABLE xapian_index ("
		"  name TEXT NOT NULL,"
		"  path TEXT NOT NULL,"
		"  table_name TEXT NOT NULL,"
		"  table_id TEXT NOT NULL,"
		"  table_data TEXT NOT NULL"
		")",
	false, 1);
	SPI_execute(
		"CREATE UNIQUE INDEX xapian_index_name ON xapian_index (name)",
	false, 1);
}


void xapian_drop_trigger(char *name, char *table)
{
	if(!xapian_trigger_exists(name))
		return;

	char sql[1024];
	sprintf(sql,
		"DROP TRIGGER %s "
		"ON %s ",
		name, table
	);
	SPI_execute(sql, false, 1);
}


//@ boolean xapian_create_index(text, text, text, text, text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_create_index); }
extern "C" Datum pg_xapian_create_index(PG_FUNCTION_ARGS)
{
	char *name = PG_GETARG_NTSTRING(0);
	char *path = PG_GETARG_NTSTRING(1);
	char *table = PG_GETARG_NTSTRING(2);
	char *doc_id_field = PG_GETARG_NTSTRING(3);
	char *doc_data_field = PG_GETARG_NTSTRING(4);
	char sql[1024];
	char trigger_name[64];
	SPI_connect();
	
	// create the xapian_index table
	xapian_create_system_tables();
	
	// create the database
	XAPIAN_CATCH_BEGIN
		Xapian::WritableDatabase database(path, Xapian::DB_CREATE_OR_OPEN);
	XAPIAN_CATCH_END
	
	// register the index
	sprintf(sql,
		"INSERT INTO xapian_index (name, path, table_name, table_id, table_data) "
		"VALUES ('%s', '%s', '%s', '%s', '%s')",
		name, path, table, doc_id_field, doc_data_field
	);
	SPI_execute(sql, false, 1);
	
	// INSERT trigger
	sprintf(sql,
		"CREATE OR REPLACE FUNCTION %s_insert_trigger() RETURNS TRIGGER AS $$\n"
		"BEGIN\n"
		"  NEW.%s := xapian_add_document('%s'::text, NEW.%s::text);\n"
		"  RETURN NEW;\n"
		"END;\n"
		"$$ LANGUAGE PLPGSQL",
		name, doc_id_field, name, doc_data_field
	);
	SPI_execute(sql, false, 1);
	
	sprintf(trigger_name, "%s_trigger_insert", name);
	xapian_drop_trigger(trigger_name, table);
	sprintf(sql,
		"CREATE TRIGGER %s "
		"BEFORE INSERT ON %s "
		"FOR EACH ROW EXECUTE PROCEDURE %s_insert_trigger()",
		trigger_name, table, name
	);
	SPI_execute(sql, false, 1);
	
	// UPDATE trigger
	sprintf(sql,
		"CREATE OR REPLACE FUNCTION %s_update_trigger() RETURNS TRIGGER AS $$\n"
		"BEGIN\n"
		"  PERFORM xapian_update_document('%s'::text, NEW.%s, NEW.%s::text);\n"
		"  RETURN NEW;\n"
		"END;\n"
		"$$ LANGUAGE PLPGSQL",
		name, name, doc_id_field, doc_data_field
	);
	SPI_execute(sql, false, 1);
	
	sprintf(trigger_name, "%s_update_insert", name);
	xapian_drop_trigger(trigger_name, table);
	sprintf(sql,
		"CREATE TRIGGER %s "
		"BEFORE UPDATE ON %s "
		"FOR EACH ROW EXECUTE PROCEDURE %s_update_trigger()",
		trigger_name, table, name
	);
	SPI_execute(sql, false, 1);
	
	// DELETE trigger
	sprintf(sql,
		"CREATE OR REPLACE FUNCTION %s_delete_trigger() RETURNS TRIGGER AS $$\n"
		"BEGIN\n"
		"  PERFORM xapian_delete_document('%s'::text, OLD.%s);\n"
		"  RETURN OLD;\n"
		"END;\n"
		"$$ LANGUAGE PLPGSQL",
		name, name, doc_id_field
	);
	SPI_execute(sql, false, 1);
	
	sprintf(trigger_name, "%s_delete_insert", name);
	xapian_drop_trigger(trigger_name, table);
	sprintf(sql,
		"CREATE TRIGGER %s "
		"BEFORE DELETE ON %s "
		"FOR EACH ROW EXECUTE PROCEDURE %s_delete_trigger()",
		trigger_name, table, name
	);
	SPI_execute(sql, false, 1);
	
	// success
	SPI_finish();
	PG_RETURN_BOOL(true);
}


//@ int xapian_add_document(text, text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_add_document); }
extern "C" Datum pg_xapian_add_document(PG_FUNCTION_ARGS)
{
	char *name = PG_GETARG_NTSTRING(0);
	char *data = PG_GETARG_NTSTRING(1);
	SPI_connect();
	char *path = xapian_get_index_path(name);
	SPI_finish();
	
	XAPIAN_CATCH_BEGIN
		// open the database
		Xapian::WritableDatabase database(path, Xapian::DB_OPEN);
		
		// create the document with parsed terms
		Xapian::Document doc;
		Xapian::TermGenerator terms;
		terms.set_document(doc);
		terms.index_text(data);
		
		// attempt to add the document
		Xapian::docid id = database.add_document(doc);
		PG_RETURN_UINT32(id);
	XAPIAN_CATCH_END
	
	PG_RETURN_NULL();
}


//@ text xapian_get_document(text, int)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_get_document); }
extern "C" Datum pg_xapian_get_document(PG_FUNCTION_ARGS)
{
	char *name = PG_GETARG_NTSTRING(0);
	int doc_id = PG_GETARG_INT32(1);
	SPI_connect();
	char *path = xapian_get_index_path(name);
	SPI_finish();
	
	XAPIAN_CATCH_BEGIN
		// open the database
		Xapian::WritableDatabase database(path, Xapian::DB_OPEN);
		
		// get the document description
		Xapian::Document doc = database.get_document(doc_id);
		string desc = "";
		for(Xapian::TermIterator i = doc.termlist_begin(); i != doc.termlist_end(); ++i)
			desc += *i + " ";
		char *description = (char*) desc.c_str();
		
		text *t = (text*) palloc(VARHDRSZ + strlen(description));
		SET_VARSIZE(t, VARHDRSZ + strlen(description));
		memcpy(VARDATA(t), description, strlen(description));
	    
	    PG_RETURN_TEXT_P(t);
	XAPIAN_CATCH_END
	
	PG_RETURN_NULL();
}


//@ boolean xapian_update_document(text, int, text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_update_document); }
extern "C" Datum pg_xapian_update_document(PG_FUNCTION_ARGS)
{
	char *name = PG_GETARG_NTSTRING(0);
	int doc_id = PG_GETARG_INT32(1);
	char *data = PG_GETARG_NTSTRING(2);
	SPI_connect();
	char *path = xapian_get_index_path(name);
	SPI_finish();
	
	XAPIAN_CATCH_BEGIN
		// open the database
		Xapian::WritableDatabase database(path, Xapian::DB_OPEN);
		
		// create the document with parsed terms
		Xapian::Document doc;
		Xapian::TermGenerator terms;
		terms.set_document(doc);
		terms.index_text(data);
		
		// attempt to update the document
		database.replace_document(doc_id, doc);
		PG_RETURN_BOOL(true);
	XAPIAN_CATCH_END
	
	PG_RETURN_NULL();
}


//@ boolean xapian_delete_document(text, int)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_delete_document); }
extern "C" Datum pg_xapian_delete_document(PG_FUNCTION_ARGS)
{
	char *name = PG_GETARG_NTSTRING(0);
	int doc_id = PG_GETARG_INT32(1);
	SPI_connect();
	char *path = xapian_get_index_path(name);
	SPI_finish();
	
	XAPIAN_CATCH_BEGIN
		// open the database
		Xapian::WritableDatabase database(path, Xapian::DB_OPEN);
		
		// attempt to delete the document
		database.delete_document(doc_id);
		PG_RETURN_BOOL(true);
	XAPIAN_CATCH_END
	
	PG_RETURN_NULL();
}


//@ int xapian_count_documents(text)
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_count_documents); }
extern "C" Datum pg_xapian_count_documents(PG_FUNCTION_ARGS)
{
	SPI_connect();
	char *path = xapian_get_index_path(PG_GETARG_NTSTRING(0));
	SPI_finish();
	
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
extern "C" { PG_FUNCTION_INFO_V1(pg_xapian_match); }
extern "C" Datum pg_xapian_match(PG_FUNCTION_ARGS)
{
	SPI_connect();
	char *path = xapian_get_index_path(PG_GETARG_NTSTRING(0));
	SPI_finish();
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

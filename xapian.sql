CREATE OR REPLACE FUNCTION xapian_install()
RETURNS BOOLEAN AS $$
DECLARE
  pgtype pg_type%rowtype;
BEGIN
  -- create xapian_document type
  SELECT * INTO pgtype FROM pg_type WHERE typname='xapian_document';
  IF NOT FOUND THEN
    CREATE TYPE xapian_document AS ( document_id INT, relevance INT );
  END IF;
  
  RETURN true;
END;
$$ LANGUAGE plpgsql;


CREATE OR REPLACE FUNCTION xapian(index_name text, terms text)
RETURNS SETOF xapian_document AS $$
DECLARE
  result xapian_document%rowtype;
BEGIN
  FOR result IN SELECT * FROM xapian_match(index_name, terms) as (id INT, rel INT) LOOP
    RETURN NEXT result;
  END LOOP;
  RETURN;
END;
$$ LANGUAGE plpgsql;

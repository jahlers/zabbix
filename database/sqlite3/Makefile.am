if DBSCHEMA
DATABASE = sqlite3

data.sql: $(top_srcdir)/create/src/data.tmpl $(top_srcdir)/create/src/templates.tmpl $(top_srcdir)/create/src/dashboards.tmpl
	$(top_srcdir)/create/bin/gen_data.pl $(DATABASE) > data.sql

schema.sql: $(top_srcdir)/create/src/schema.tmpl
	$(top_srcdir)/create/bin/gen_schema.pl $(DATABASE) > schema.sql
endif

EXTRA_DIST = \
	data.sql \
	images.sql \
	schema.sql

clean:
	rm -f schema.sql data.sql

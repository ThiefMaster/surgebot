CREATE TABLE title_history
(
	id serial NOT NULL,
	artist character varying,
	title character varying NOT NULL,
	mod character varying,
	listeners smallint NOT NULL,
	ts timestamp without time zone NOT NULL DEFAULT timezone('UTC'::text, now()),
	CONSTRAINT titlehistory_pkey PRIMARY KEY (id)
)
WITH (
	OIDS=FALSE
);

CREATE INDEX title_history_artist_idx ON title_history USING btree (artist);
CREATE INDEX title_history_mod_idx ON title_history USING btree (mod);
CREATE INDEX title_history_title_idx ON title_history USING btree (title);

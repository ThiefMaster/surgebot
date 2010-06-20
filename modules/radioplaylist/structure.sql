CREATE TABLE playlist
(
	id serial NOT NULL,
	file bytea NOT NULL,
	artist character varying,
	album character varying,
	title character varying,
	duration smallint NOT NULL,
	blacklist boolean NOT NULL DEFAULT false,
	st_inode integer NOT NULL,
	st_size integer NOT NULL,
	st_mtime integer NOT NULL,
	CONSTRAINT playlist_pkey PRIMARY KEY (id)
)
WITH (
	OIDS=FALSE
);

CREATE INDEX playlist_blacklist_index ON playlist USING btree (blacklist);
CREATE INDEX playlist_duration_idx ON playlist USING btree (duration);
CREATE UNIQUE INDEX playlist_file_idx ON playlist USING btree (file);

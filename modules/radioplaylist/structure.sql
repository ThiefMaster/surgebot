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
	last_vote integer NOT NULL DEFAULT 0,
	CONSTRAINT playlist_pkey PRIMARY KEY (id)
)
WITH (
	OIDS=FALSE
);

CREATE INDEX playlist_blacklist_index ON playlist USING btree (blacklist);
CREATE INDEX playlist_duration_idx ON playlist USING btree (duration);
CREATE UNIQUE INDEX playlist_file_idx ON playlist USING btree (file);



CREATE TABLE genres
(
	id serial NOT NULL,
	genre character varying(32) NOT NULL,
	min_votes smallint NOT NULL DEFAULT 1,
	public boolean NOT NULL DEFAULT true,
	description character varying,
	sortorder smallint NOT NULL DEFAULT 0,
	CONSTRAINT genres_pkey PRIMARY KEY (id)
)
WITH (
	OIDS=FALSE
);

CREATE UNIQUE INDEX genre_unique_ci ON genres USING btree (lower(genre::text));



CREATE TABLE song_genres
(
	genre_id integer NOT NULL,
	song_id integer NOT NULL,
	CONSTRAINT song_genres_pkey PRIMARY KEY (genre_id, song_id),
	CONSTRAINT song_genres_genre_id_fkey FOREIGN KEY (genre_id) REFERENCES genres (id) MATCH SIMPLE ON UPDATE NO ACTION ON DELETE CASCADE,
	CONSTRAINT song_genres_song_id_fkey FOREIGN KEY (song_id) REFERENCES playlist (id) MATCH SIMPLE ON UPDATE NO ACTION ON DELETE CASCADE
)
WITH (
	OIDS=FALSE
);



CREATE TABLE history
(
	id serial NOT NULL,
	song_id integer NOT NULL,
	ts timestamp without time zone NOT NULL DEFAULT now(),
	CONSTRAINT history_pkey PRIMARY KEY (id),
	CONSTRAINT history_song_id_fkey FOREIGN KEY (song_id) REFERENCES playlist (id) MATCH SIMPLE ON UPDATE NO ACTION ON DELETE CASCADE
)
WITH (
	OIDS=FALSE
);

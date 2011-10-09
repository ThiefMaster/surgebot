CREATE TABLE playlist_songs
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
	promo boolean NOT NULL DEFAULT false,
	jingle boolean NOT NULL DEFAULT false,
	CONSTRAINT playlist_songs_pkey PRIMARY KEY (id),
	CONSTRAINT playlist_songs_file_key UNIQUE (file)
)
WITH (
	OIDS=FALSE
);

CREATE INDEX ix_playlist_songs_blacklist ON playlist_songs USING btree (blacklist);
CREATE INDEX ix_playlist_songs_duration ON playlist_songs USING btree (duration);
CREATE INDEX ix_playlist_songs_promo ON playlist_songs USING btree (promo);
CREATE INDEX ix_playlist_songs_promook ON playlist_songs USING btree (last_vote) WHERE promo AND NOT blacklist;



CREATE TABLE playlist_genres
(
	id serial NOT NULL,
	genre character varying(32) NOT NULL,
	min_votes smallint NOT NULL DEFAULT 1,
	public boolean NOT NULL DEFAULT true,
	description character varying,
	sortorder smallint NOT NULL DEFAULT 0,
	CONSTRAINT playlist_genres_pkey PRIMARY KEY (id)
)
WITH (
	OIDS=FALSE
);

CREATE UNIQUE INDEX genre_unique_ci ON genres USING btree (lower(genre::text));



CREATE TABLE playlist_song_genres
(
	genre_id integer NOT NULL,
	song_id integer NOT NULL,
	CONSTRAINT playlist_song_genres_pkey PRIMARY KEY (genre_id, song_id),
	CONSTRAINT playlist_song_genres_genre_id_fkey FOREIGN KEY (genre_id) REFERENCES playlist_genres (id) MATCH SIMPLE ON UPDATE NO ACTION ON DELETE CASCADE,
	CONSTRAINT playlist_song_genres_song_id_fkey FOREIGN KEY (song_id) REFERENCES playlist_playlist (id) MATCH SIMPLE ON UPDATE NO ACTION ON DELETE CASCADE
)
WITH (
	OIDS=FALSE
);

CREATE INDEX ix_playlist_song_genres_genre_id ON playlist_song_genres USING btree (genre_id);
CREATE INDEX ix_playlist_song_genres_song_id ON playlist_song_genres USING btree (song_id);



CREATE TABLE playlist_history
(
	id serial NOT NULL,
	song_id integer NOT NULL,
	ts timestamp without time zone NOT NULL DEFAULT timezone('UTC'::text, now()),
	CONSTRAINT playlist_history_pkey PRIMARY KEY (id),
	CONSTRAINT playlist_history_song_id_fkey FOREIGN KEY (song_id) REFERENCES playlist_songs (id) MATCH SIMPLE ON UPDATE NO ACTION ON DELETE CASCADE
)
WITH (
	OIDS=FALSE
);

CREATE INDEX ix_playlist_history_song_id ON playlist_history USING btree (song_id);
CREATE INDEX ix_playlist_history_ts ON playlist_history USING btree (ts);

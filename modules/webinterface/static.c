#include "global.h"
#include "modules/httpd/http.h"
#include "modules/tools/tools.h"
#include "static.h"

HTTP_HANDLER(static_dir_handler);
HTTP_HANDLER(static_handler);

static struct http_handler handlers[] = {
	{ "/", static_handler },
	{ "/static/", static_dir_handler },
	{ "/static/?*", static_handler },
	{ NULL, NULL }
};

static struct static_file {
	const char *virtual;
	const char *file;
	const char *content_type;
} static_files[] = {
	// javascript
	{ "jquery.js", "modules/webinterface/files/jquery-1.2.6.js", "text/javascript" },
	{ "jquery.form.js", "modules/webinterface/files/jquery.form.js", "text/javascript" },
	{ "jquery.validate.js", "modules/webinterface/files/jquery.validate.js", "text/javascript" },
	{ "rsh.js", "modules/webinterface/files/rsh.js", "text/javascript" },
	{ "json2005.js", "modules/webinterface/files/json2005.js", "text/javascript" },
	// css
	{ "style.css", "modules/webinterface/files/style.css", "text/css" },
	// images
	{ "loading.gif", "modules/webinterface/files/loading.gif", "image/gif" },
	{ "loadingMenu.gif", "modules/webinterface/files/loadingMenu.gif", "image/gif" },
	// html
	{ "rsh-blank.html*", "modules/webinterface/files/rsh-blank.html", "text/html" }, // used by rsh.js
	{ "$INDEX$", "modules/webinterface/files/index.html", "text/html" },
};

void static_init()
{
	http_handler_add_list(handlers);
}

void static_fini()
{
	http_handler_del_list(handlers);
}

HTTP_HANDLER(static_dir_handler)
{
	size_t maxlen = 0;
	unsigned int i;

	for(i = 0; i < ArraySize(static_files); i++)
	{
		size_t tmp = strlen(static_files[i].virtual);
		if(tmp > maxlen)
			maxlen = tmp;
	}

	maxlen += 10;

	http_reply_header("Content-Type", "text/html");
	http_reply("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\n");
	http_reply("<html>\n");
	http_reply(" <head>\n");
	http_reply("  <title>Index of /%s</title>\n", argv[0]);
	http_reply(" </head>\n");
	http_reply(" <body>\n");
	http_reply("  <h1>Index of /%s</h1>\n", argv[0]);
	http_reply("  <hr>\n");
	http_reply("  <pre>\n");
	for(i = 0; i < ArraySize(static_files); i++)
	{
		struct static_file *file = &static_files[i];
		if(!strcmp(file->virtual, "$INDEX$"))
			continue;
		http_reply("* <a href='/static/%s'>%s</a>%*s%s\n", file->virtual, file->virtual, (int)(maxlen - strlen(file->virtual)), "", file->content_type);
	}
	http_reply("  </pre>\n");
	http_reply(" </body>\n");
	http_reply("</html>");
}

HTTP_HANDLER(static_handler)
{
	struct static_file *file = NULL;
	char *filename;
	char buf[4096], nowbuf[64], modbuf[64];
	time_t mod;
	struct stat sbuf;
	FILE *fd;

	filename = argc > 0 ? argv[argc - 1] : "$INDEX$";
	for(unsigned int i = 0; i < ArraySize(static_files); i++)
	{
		if(!match(static_files[i].virtual, filename))
		{
			file = &static_files[i];
			break;
		}
	}

	if(!file)
	{
		http_write_header_status(client, 404);
		http_reply_header("Content-Type", "text/html");
		filename = html_encode(filename);
		http_reply("File not found: %s", filename);
		free(filename);
		return;
	}

	fd = fopen(file->file, "r");
	if(!fd)
	{
		http_write_header_status(client, 404);
		http_reply_header("Content-Type", "text/html");
		filename = html_encode(filename);
		http_reply("File '%s' could not be opened for reading: %s (%d)", file->file, strerror(errno), errno);
		free(filename);
		return;
	}

	if(stat(file->file, &sbuf) != 0)
	{
		fclose(fd);
		http_write_header_status(client, 404);
		http_reply_header("Content-Type", "text/html");
		filename = html_encode(filename);
		http_reply("File '%s' could not be stat()'d: %s (%d)", file->file, strerror(errno), errno);
		free(filename);
		return;
	}

	mod = sbuf.st_mtime ? sbuf.st_mtime : now;
	strftime(nowbuf, sizeof(nowbuf), RFC1123FMT, gmtime(&now));
	strftime(modbuf, sizeof(modbuf), RFC1123FMT, gmtime(&mod));
	mod = mktime(gmtime(&mod));

	http_reply_header("Date", nowbuf);
	http_reply_header("Last-Modified", modbuf);
	http_reply_header("Content-Type", file->content_type);
	http_reply_header("Cache-Control", "must-revalidate");

	if(mod <= client->if_modified_since) // same as cached version -> send 304 instead of file
	{
		debug("Sending 304 for %s", client->uri);
		http_write_header_status(client, 304);
		fclose(fd);
		return;
	}

	while(!feof(fd))
	{
		size_t len = fread(buf, 1, sizeof(buf), fd);
		stringbuffer_append_string_n(client->wbuf, buf, len);
	}
	fclose(fd);
}

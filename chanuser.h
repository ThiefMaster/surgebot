#ifndef HAVE_CHANUSER_H
#define HAVE_CHANUSER_H

#include "hook.h"

#define MODE_VOICE		0x00001 /* +v */
#define MODE_OP			0x00002 /* +o */

#define MODE_KEYED		0x00004 /* +k */
#define MODE_INVITEONLY		0x00008 /* +i */
#define MODE_PRIVATE		0x00010 /* +p */
#define MODE_SECRET		0x00020 /* +s */
#define MODE_MODERATED		0x00040 /* +m */
#define MODE_LIMIT		0x00080 /* +l */
#define MODE_TOPICLIMIT		0x00100 /* +t */
#define MODE_NOPRIVMSGS		0x00200 /* +n */
#define MODE_DELJOINS		0x00400 /* +D */
#define MODE_WASDELJOIN		0x00800 /* +d */
#define MODE_REGONLY		0x01000 /* +r */
#define MODE_NOCOLOUR		0x02000 /* +c */
#define MODE_NOCTCP		0x04000 /* +C */
#define MODE_REGISTERED		0x08000 /* +z */

#define BURST_FINISHED	0x00
#define BURST_NAMES	0x01
#define BURST_MODES	0x02
#define BURST_BANS	0x04
#define BURST_WHO	0x08

#define DEL_PART	0x1
#define DEL_KICK	0x2
#define DEL_QUIT	0x3

void chanuser_init();
void chanuser_fini();
void chanuser_flush();

struct dict *channel_dict();
struct irc_channel* channel_add(const char *name, int do_burst);
void channel_complete(struct irc_channel *channel);
struct irc_channel* channel_find(const char *name);
void channel_del(struct irc_channel *channel, unsigned int del_type, const char *reason);
void channel_set_topic(struct irc_channel *channel, const char *topic, time_t ts);
void channel_set_key(struct irc_channel *channel, const char *key);
void channel_set_limit(struct irc_channel *channel, unsigned int limit);

struct dict *user_dict();
struct irc_user* user_add(const char *nick, const char *ident, const char *host);
struct irc_user* user_add_nick(const char *nick);
void user_complete(struct irc_user *user, const char *ident, const char *host);
void user_set_info(struct irc_user *user, const char *info);
struct irc_user* user_find(const char *nick);
void user_del(struct irc_user *user, unsigned int del_type, const char *reason);
void user_rename(struct irc_user *user, const char *nick);

struct irc_chanuser* channel_user_add(struct irc_channel *channel, struct irc_user *user, int flags);
struct irc_chanuser* channel_user_find(struct irc_channel *channel, struct irc_user *user);
int channel_user_del(struct irc_channel *channel, struct irc_user *user, unsigned int del_type, int check_dead, const char *reason);

struct irc_ban* channel_ban_add(struct irc_channel *channel, const char *mask);
struct irc_ban* channel_ban_find(struct irc_channel *channel, const char *mask);
void channel_ban_del(struct irc_channel *channel, const char *mask);

char *get_mode_char(struct irc_chanuser *cuser);

DECLARE_HOOKABLE(channel_del, (struct irc_channel *channel, const char *reason));
DECLARE_HOOKABLE(channel_complete, (struct irc_channel *channel));
DECLARE_HOOKABLE(user_del, (struct irc_user *user, unsigned int quit, const char *reason));
DECLARE_HOOKABLE(chanuser_del, (struct irc_chanuser *user, unsigned int del_type, const char *reason));

#endif

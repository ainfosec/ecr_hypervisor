/* 
    Domain communications for Xen Store Daemon.
    Copyright (C) 2005 Rusty Russell IBM Corporation

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _XENSTORED_DOMAIN_H
#define _XENSTORED_DOMAIN_H

void handle_event(void);

/* domid, mfn, eventchn, path */
int do_introduce(struct connection *conn, struct buffered_data *in);

/* domid */
int do_is_domain_introduced(struct connection *conn, struct buffered_data *in);

/* domid */
int do_release(struct connection *conn, struct buffered_data *in);

/* domid */
int do_resume(struct connection *conn, struct buffered_data *in);

/* domid, target */
int do_set_target(struct connection *conn, struct buffered_data *in);

/* domid */
int do_get_domain_path(struct connection *conn, struct buffered_data *in);

/* Allow guest to reset all watches */
int do_reset_watches(struct connection *conn, struct buffered_data *in);

void domain_init(void);

/* Returns the implicit path of a connection (only domains have this) */
const char *get_implicit_path(const struct connection *conn);

/* Read existing connection information from store. */
void restore_existing_connections(void);

/* Can connection attached to domain read/write. */
bool domain_can_read(struct connection *conn);
bool domain_can_write(struct connection *conn);

bool domain_is_unprivileged(struct connection *conn);

/* Quota manipulation */
void domain_entry_inc(struct connection *conn, struct node *);
void domain_entry_dec(struct connection *conn, struct node *);
int domain_entry_fix(unsigned int domid, int num, bool update);
int domain_entry(struct connection *conn);
void domain_watch_inc(struct connection *conn);
void domain_watch_dec(struct connection *conn);
int domain_watch(struct connection *conn);

/* Write rate limiting */

#define WRL_FACTOR   1000 /* for fixed-point arithmetic */
#define WRL_RATE      200
#define WRL_DBURST     10
#define WRL_GBURST   1000
#define WRL_NEWDOMS     5
#define WRL_LOGEVERY  120 /* seconds */

struct wrl_timestampt {
	time_t sec;
	int msec;
};

extern long wrl_ntransactions;

void wrl_gettime_now(struct wrl_timestampt *now_ts);
void wrl_domain_new(struct domain *domain);
void wrl_domain_destroy(struct domain *domain);
void wrl_credit_update(struct domain *domain, struct wrl_timestampt now);
void wrl_check_timeout(struct domain *domain,
                       struct wrl_timestampt now,
                       int *ptimeout);
void wrl_log_periodic(struct wrl_timestampt now);
void wrl_apply_debit_direct(struct connection *conn);
void wrl_apply_debit_trans_commit(struct connection *conn);

#endif /* _XENSTORED_DOMAIN_H */

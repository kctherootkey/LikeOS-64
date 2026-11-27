/*
 * pam.c - minimal PAM for LikeOS (libpam.so).
 *
 * No /etc/pam.d and no loadable modules: each service gets one of a few
 * built-in policies, the ones a conventional configuration would give it.
 *
 *   "lightdm-greeter"    the display manager's greeter runs as its own
 *                        system account without a password: authentication
 *                        and account management always succeed.
 *   "lightdm-autologin"  a login without a password, refused for an
 *                        account whose shell says it may not log in
 *                        (nologin, false); the account check is the
 *                        password services' one.
 *   everything else      "unix": authentication verifies the password
 *                        against /etc/shadow via crypt(); account
 *                        management rejects unknown or locked accounts.
 *
 * A login service ("login", "lightdm", "lightdm-greeter",
 * "lightdm-autologin") records the login when the session is opened: the
 * calling process, which must be root, writes the user's uid to
 * /proc/self/loginuid -- the kernel then treats it and everything it starts
 * as that user's login, with a login session id of its own (the console
 * user, the power commands).  Other services (su, passwd) do not: they act
 * within the login they were started from.  Credentials are not
 * established by PAM (the application switches uid/gid itself), so
 * pam_setcred is a no-op, as is pam_close_session.
 *
 * Copyright (C) 2026 The LikeOS Project
 */

#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <shadow.h>
#include <pwd.h>
#include <crypt.h>

#ifndef PAM_LOGINUID_PATH
#define PAM_LOGINUID_PATH "/proc/self/loginuid"
#endif

enum pam_policy {
	POLICY_UNIX,      /* password against /etc/shadow */
	POLICY_PERMIT,    /* no authentication, no account check */
	POLICY_AUTOLOGIN, /* no password; shell must allow a login */
};

struct pam_data_entry {
	char *name;
	void *data;
	void (*cleanup)(pam_handle_t *pamh, void *data, int error_status);
	struct pam_data_entry *next;
};

struct pam_handle {
	char *service;
	char *user;
	char *authtok;
	char *oldauthtok;
	char *tty;
	char *rhost;
	char *ruser;
	char *user_prompt;
	char *xdisplay;
	char *authtok_type;
	struct pam_xauth_data xauth; /* deep copy; name/data NULL when unset */
	int have_xauth;
	pam_fail_delay_fn delay_fn;
	unsigned int fail_delay_usec;
	int fail_delay_set;
	const struct pam_conv *conv;
	char **env; /* "NAME=value", nenv entries, NULL-terminated */
	size_t nenv;
	size_t capenv;
	struct pam_data_entry *data;
	int last_status;
};

static char *pam_strdup(const char *s)
{
	return s ? strdup(s) : NULL;
}

/* Free a string that may hold a secret, clearing it first. */
static void free_secret(char *s)
{
	if (s) {
		volatile char *p = s;

		while (*p)
			*p++ = '\0';
		free(s);
	}
}

static enum pam_policy service_policy(const char *service)
{
	if (service && strcmp(service, "lightdm-greeter") == 0)
		return POLICY_PERMIT;
	if (service && strcmp(service, "lightdm-autologin") == 0)
		return POLICY_AUTOLOGIN;
	return POLICY_UNIX;
}

/* Services that start a login: their pam_open_session records it. */
static int service_records_login(const char *service)
{
	static const char *const names[] = {
		"login", "lightdm", "lightdm-greeter", "lightdm-autologin", NULL
	};

	for (int i = 0; service && names[i]; i++)
		if (strcmp(service, names[i]) == 0)
			return 1;
	return 0;
}

/* Issue one conversation message and return the response string (or NULL). */
static int converse(pam_handle_t *pamh, int style, const char *prompt,
                    char **response)
{
	struct pam_message msg;
	const struct pam_message *msgp = &msg;
	struct pam_response *resp = NULL;
	int rc;

	if (response)
		*response = NULL;
	if (!pamh->conv || !pamh->conv->conv)
		return PAM_CONV_ERR;

	msg.msg_style = style;
	msg.msg = prompt;
	rc = pamh->conv->conv(1, &msgp, &resp, pamh->conv->appdata_ptr);
	if (rc != PAM_SUCCESS)
		return rc;
	if (resp) {
		if (response)
			*response = resp->resp; /* transfer ownership */
		else if (resp->resp)
			free(resp->resp);
		free(resp);
	}
	return PAM_SUCCESS;
}

int pam_start(const char *service_name, const char *user,
              const struct pam_conv *pam_conversation, pam_handle_t **pamh)
{
	pam_handle_t *h;

	if (!pamh || !pam_conversation)
		return PAM_SYSTEM_ERR;
	h = calloc(1, sizeof(*h));
	if (!h)
		return PAM_BUF_ERR;
	h->service = pam_strdup(service_name);
	h->user = pam_strdup(user);
	if ((service_name && !h->service) || (user && !h->user)) {
		free(h->service);
		free(h->user);
		free(h);
		return PAM_BUF_ERR;
	}
	h->conv = pam_conversation;
	h->last_status = PAM_SUCCESS;
	*pamh = h;
	return PAM_SUCCESS;
}

static void clear_xauth(pam_handle_t *pamh)
{
	if (pamh->xauth.data)
		memset(pamh->xauth.data, 0, (size_t)pamh->xauth.datalen);
	free(pamh->xauth.name);
	free(pamh->xauth.data);
	memset(&pamh->xauth, 0, sizeof(pamh->xauth));
	pamh->have_xauth = 0;
}

int pam_end(pam_handle_t *pamh, int pam_status)
{
	struct pam_data_entry *d, *next;

	if (!pamh)
		return PAM_SYSTEM_ERR;
	/* Each piece of data gets its cleanup call first, while everything
	 * else on the handle is still there for it to look at. */
	for (d = pamh->data; d; d = next) {
		next = d->next;
		if (d->cleanup)
			d->cleanup(pamh, d->data, pam_status);
		free(d->name);
		free(d);
	}
	pamh->data = NULL;
	free(pamh->service);
	free(pamh->user);
	free_secret(pamh->authtok);
	free_secret(pamh->oldauthtok);
	free(pamh->tty);
	free(pamh->rhost);
	free(pamh->ruser);
	free(pamh->user_prompt);
	free(pamh->xdisplay);
	free(pamh->authtok_type);
	clear_xauth(pamh);
	for (size_t i = 0; i < pamh->nenv; i++)
		free_secret(pamh->env[i]);
	free(pamh->env);
	free(pamh);
	return pam_status;
}

static int set_xauth(pam_handle_t *pamh, const struct pam_xauth_data *x)
{
	struct pam_xauth_data copy;

	memset(&copy, 0, sizeof(copy));
	if (x) {
		if (x->namelen < 0 || x->datalen < 0 ||
		    (x->namelen && !x->name) || (x->datalen && !x->data))
			return PAM_BAD_ITEM;
		copy.namelen = x->namelen;
		copy.datalen = x->datalen;
		/* NUL-terminated as well, for callers that print the name. */
		copy.name = malloc((size_t)x->namelen + 1);
		copy.data = malloc((size_t)x->datalen + 1);
		if (!copy.name || !copy.data) {
			free(copy.name);
			free(copy.data);
			return PAM_BUF_ERR;
		}
		if (x->namelen)
			memcpy(copy.name, x->name, (size_t)x->namelen);
		copy.name[x->namelen] = '\0';
		if (x->datalen)
			memcpy(copy.data, x->data, (size_t)x->datalen);
		copy.data[x->datalen] = '\0';
	}
	clear_xauth(pamh);
	if (x) {
		pamh->xauth = copy;
		pamh->have_xauth = 1;
	}
	return PAM_SUCCESS;
}

int pam_set_item(pam_handle_t *pamh, int item_type, const void *item)
{
	char **slot;
	char *copy;
	int secret = 0;

	if (!pamh)
		return PAM_SYSTEM_ERR;
	switch (item_type) {
	case PAM_SERVICE:     slot = &pamh->service; break;
	case PAM_USER:        slot = &pamh->user; break;
	case PAM_TTY:         slot = &pamh->tty; break;
	case PAM_RHOST:       slot = &pamh->rhost; break;
	case PAM_RUSER:       slot = &pamh->ruser; break;
	case PAM_USER_PROMPT: slot = &pamh->user_prompt; break;
	case PAM_XDISPLAY:    slot = &pamh->xdisplay; break;
	case PAM_AUTHTOK_TYPE: slot = &pamh->authtok_type; break;
	case PAM_AUTHTOK:     slot = &pamh->authtok; secret = 1; break;
	case PAM_OLDAUTHTOK:  slot = &pamh->oldauthtok; secret = 1; break;
	case PAM_CONV:
		/* A handle without a conversation could never ask anything. */
		if (!item)
			return PAM_PERM_DENIED;
		pamh->conv = (const struct pam_conv *)item;
		return PAM_SUCCESS;
	case PAM_FAIL_DELAY:
		pamh->delay_fn = (pam_fail_delay_fn)item;
		return PAM_SUCCESS;
	case PAM_XAUTHDATA:
		return set_xauth(pamh, (const struct pam_xauth_data *)item);
	default:
		return PAM_BAD_ITEM;
	}
	copy = pam_strdup((const char *)item);
	if (item && !copy)
		return PAM_BUF_ERR;
	if (secret)
		free_secret(*slot);
	else
		free(*slot);
	*slot = copy;
	return PAM_SUCCESS;
}

int pam_get_item(const pam_handle_t *pamh, int item_type, const void **item)
{
	if (!pamh || !item)
		return PAM_SYSTEM_ERR;
	switch (item_type) {
	case PAM_SERVICE:     *item = pamh->service; break;
	case PAM_USER:        *item = pamh->user; break;
	case PAM_TTY:         *item = pamh->tty; break;
	case PAM_RHOST:       *item = pamh->rhost; break;
	case PAM_RUSER:       *item = pamh->ruser; break;
	case PAM_USER_PROMPT: *item = pamh->user_prompt; break;
	case PAM_AUTHTOK:     *item = pamh->authtok; break;
	case PAM_OLDAUTHTOK:  *item = pamh->oldauthtok; break;
	case PAM_CONV:        *item = pamh->conv; break;
	case PAM_XDISPLAY:    *item = pamh->xdisplay; break;
	case PAM_AUTHTOK_TYPE: *item = pamh->authtok_type; break;
	case PAM_FAIL_DELAY:  *item = (const void *)pamh->delay_fn; break;
	case PAM_XAUTHDATA:
		*item = pamh->have_xauth ? &pamh->xauth : NULL;
		break;
	default:
		return PAM_BAD_ITEM;
	}
	return PAM_SUCCESS;
}

int pam_get_user(pam_handle_t *pamh, const char **user, const char *prompt)
{
	char *resp = NULL;
	const char *p;
	int rc;

	if (!pamh || !user)
		return PAM_SYSTEM_ERR;
	if (pamh->user && pamh->user[0]) {
		*user = pamh->user;
		return PAM_SUCCESS;
	}
	p = prompt ? prompt : (pamh->user_prompt ? pamh->user_prompt : "login: ");
	rc = converse(pamh, PAM_PROMPT_ECHO_ON, p, &resp);
	if (rc != PAM_SUCCESS)
		return rc;
	if (!resp)
		return PAM_CONV_ERR;
	free(pamh->user);
	pamh->user = resp;
	*user = pamh->user;
	return PAM_SUCCESS;
}

/* ---- the environment ------------------------------------------------- */

/* Index of NAME (length len) in the list, or -1. */
static long env_find(const pam_handle_t *pamh, const char *name, size_t len)
{
	for (size_t i = 0; i < pamh->nenv; i++)
		if (strncmp(pamh->env[i], name, len) == 0 &&
		    pamh->env[i][len] == '=')
			return (long)i;
	return -1;
}

int pam_putenv(pam_handle_t *pamh, const char *name_value)
{
	const char *eq;
	size_t len;
	long at;
	char *copy;

	if (!pamh || !name_value)
		return PAM_PERM_DENIED;
	eq = strchr(name_value, '=');
	len = eq ? (size_t)(eq - name_value) : strlen(name_value);
	if (len == 0)
		return PAM_BAD_ITEM;
	at = env_find(pamh, name_value, len);

	if (!eq) {
		/* "NAME": remove it. */
		if (at < 0)
			return PAM_BAD_ITEM;
		free_secret(pamh->env[at]);
		pamh->env[at] = pamh->env[pamh->nenv - 1];
		pamh->env[--pamh->nenv] = NULL;
		return PAM_SUCCESS;
	}

	copy = strdup(name_value);
	if (!copy)
		return PAM_BUF_ERR;
	if (at >= 0) {
		free_secret(pamh->env[at]);
		pamh->env[at] = copy;
		return PAM_SUCCESS;
	}
	if (pamh->nenv + 1 >= pamh->capenv) {
		size_t cap = pamh->capenv ? pamh->capenv * 2 : 16;
		char **n = realloc(pamh->env, cap * sizeof(*n));

		if (!n) {
			free(copy);
			return PAM_BUF_ERR;
		}
		pamh->env = n;
		pamh->capenv = cap;
	}
	pamh->env[pamh->nenv++] = copy;
	pamh->env[pamh->nenv] = NULL;
	return PAM_SUCCESS;
}

const char *pam_getenv(pam_handle_t *pamh, const char *name)
{
	size_t len;
	long at;

	if (!pamh || !name)
		return NULL;
	len = strlen(name);
	if (len == 0 || strchr(name, '='))
		return NULL;
	at = env_find(pamh, name, len);
	return at < 0 ? NULL : pamh->env[at] + len + 1;
}

char **pam_getenvlist(pam_handle_t *pamh)
{
	char **list;

	if (!pamh)
		return NULL;
	list = calloc(pamh->nenv + 1, sizeof(*list));
	if (!list)
		return NULL;
	for (size_t i = 0; i < pamh->nenv; i++) {
		list[i] = strdup(pamh->env[i]);
		if (!list[i]) {
			while (i > 0)
				free_secret(list[--i]);
			free(list);
			return NULL;
		}
	}
	return list;
}

/* ---- data kept with the handle --------------------------------------- */

int pam_set_data(pam_handle_t *pamh, const char *module_data_name, void *data,
                 void (*cleanup)(pam_handle_t *pamh, void *data,
                                 int error_status))
{
	struct pam_data_entry *d;

	if (!pamh || !module_data_name)
		return PAM_SYSTEM_ERR;
	for (d = pamh->data; d; d = d->next) {
		if (strcmp(d->name, module_data_name) == 0) {
			if (d->cleanup)
				d->cleanup(pamh, d->data,
					   PAM_DATA_REPLACE | PAM_SUCCESS);
			d->data = data;
			d->cleanup = cleanup;
			return PAM_SUCCESS;
		}
	}
	d = calloc(1, sizeof(*d));
	if (!d)
		return PAM_BUF_ERR;
	d->name = strdup(module_data_name);
	if (!d->name) {
		free(d);
		return PAM_BUF_ERR;
	}
	d->data = data;
	d->cleanup = cleanup;
	d->next = pamh->data;
	pamh->data = d;
	return PAM_SUCCESS;
}

int pam_get_data(const pam_handle_t *pamh, const char *module_data_name,
                 const void **data)
{
	const struct pam_data_entry *d;

	if (!pamh || !module_data_name || !data)
		return PAM_SYSTEM_ERR;
	for (d = pamh->data; d; d = d->next) {
		if (strcmp(d->name, module_data_name) == 0) {
			*data = d->data;
			return PAM_SUCCESS;
		}
	}
	return PAM_NO_MODULE_DATA;
}

/* ---- the failure delay ----------------------------------------------- */

int pam_fail_delay(pam_handle_t *pamh, unsigned int usec)
{
	if (!pamh)
		return PAM_SYSTEM_ERR;
	if (!pamh->fail_delay_set || usec > pamh->fail_delay_usec)
		pamh->fail_delay_usec = usec;
	pamh->fail_delay_set = 1;
	return PAM_SUCCESS;
}

/* After an authentication: a failure waits for the delay asked for (or
 * hands it to the application's PAM_FAIL_DELAY function); either way the
 * request is used up. */
static void await_fail_delay(pam_handle_t *pamh, int status)
{
	if (pamh->fail_delay_set && status != PAM_SUCCESS &&
	    pamh->fail_delay_usec) {
		if (pamh->delay_fn) {
			pamh->delay_fn(status, pamh->fail_delay_usec,
				       pamh->conv ? pamh->conv->appdata_ptr
						  : NULL);
		} else {
			struct timespec ts, rem;

			ts.tv_sec = pamh->fail_delay_usec / 1000000u;
			ts.tv_nsec = (long)(pamh->fail_delay_usec % 1000000u) *
				     1000L;
			while (nanosleep(&ts, &rem) != 0 && errno == EINTR)
				ts = rem;
		}
	}
	pamh->fail_delay_set = 0;
	pamh->fail_delay_usec = 0;
}

/* ---- authentication and accounts ------------------------------------- */

/* Look up the stored hash for `user`.  Returns a malloc'd copy or NULL. */
static char *lookup_hash(const char *user, int *unknown)
{
	struct spwd *sp;
	struct passwd *pw;

	*unknown = 0;
	pw = getpwnam(user);
	if (!pw) {
		*unknown = 1;
		return NULL;
	}
	sp = getspnam(user);
	if (sp && sp->sp_pwdp)
		return pam_strdup(sp->sp_pwdp);
	/* No shadow entry: fall back to the passwd field. */
	return pam_strdup(pw->pw_passwd ? pw->pw_passwd : "");
}

static int hash_is_locked(const char *hash)
{
	/* '!' or '*' prefix, or the classic "*"/"!" tokens, mean no login. */
	return hash[0] == '!' || hash[0] == '*';
}

/* A shell that says the account may not log in. */
static int shell_refuses_login(const char *shell)
{
	static const char *const refusing[] = {
		"/sbin/nologin", "/usr/sbin/nologin", "/bin/false",
		"/usr/bin/false", NULL
	};

	for (int i = 0; shell && refusing[i]; i++)
		if (strcmp(shell, refusing[i]) == 0)
			return 1;
	return 0;
}

static int authenticate_unix(pam_handle_t *pamh, const char *user)
{
	char *hash, *pw = NULL;
	struct crypt_data cd;
	char *computed;
	int unknown, rc;

	hash = lookup_hash(user, &unknown);
	if (unknown)
		return PAM_USER_UNKNOWN;
	if (!hash)
		return PAM_AUTHINFO_UNAVAIL;

	/* Empty hash => passwordless account. */
	if (hash[0] == '\0') {
		free(hash);
		return PAM_SUCCESS;
	}
	if (hash_is_locked(hash)) {
		free(hash);
		return PAM_AUTH_ERR;
	}

	rc = converse(pamh, PAM_PROMPT_ECHO_OFF, "Password: ", &pw);
	if (rc != PAM_SUCCESS) {
		free(hash);
		return rc;
	}
	if (!pw) {
		free(hash);
		return PAM_AUTH_ERR;
	}

	cd.initialized = 0;
	computed = crypt_r(pw, hash, &cd);
	rc = (computed && strcmp(computed, hash) == 0) ? PAM_SUCCESS
	                                               : PAM_AUTH_ERR;

	/* Remember the presented token so pam_setcred/chauthtok can see it. */
	if (rc == PAM_SUCCESS) {
		free_secret(pamh->authtok);
		pamh->authtok = pam_strdup(pw);
	}

	/* Scrub and free the plaintext password. */
	free_secret(pw);
	free(hash);
	return rc;
}

int pam_authenticate(pam_handle_t *pamh, int flags)
{
	const char *user = NULL;
	struct passwd *pwd;
	int rc;

	(void)flags;
	if (!pamh)
		return PAM_SYSTEM_ERR;

	rc = pam_get_user(pamh, &user, NULL);
	if (rc == PAM_SUCCESS) {
		switch (service_policy(pamh->service)) {
		case POLICY_PERMIT:
			rc = PAM_SUCCESS;
			break;
		case POLICY_AUTOLOGIN:
			pwd = getpwnam(user);
			if (!pwd)
				rc = PAM_USER_UNKNOWN;
			else if (shell_refuses_login(pwd->pw_shell))
				rc = PAM_AUTH_ERR;
			else
				rc = PAM_SUCCESS;
			break;
		default:
			rc = authenticate_unix(pamh, user);
			break;
		}
	}
	await_fail_delay(pamh, rc);
	pamh->last_status = rc;
	return rc;
}

int pam_acct_mgmt(pam_handle_t *pamh, int flags)
{
	const char *user = NULL;
	char *hash;
	int unknown, rc = PAM_SUCCESS;

	(void)flags;
	if (!pamh)
		return PAM_SYSTEM_ERR;
	if (pam_get_item(pamh, PAM_USER, (const void **)&user) != PAM_SUCCESS ||
	    !user)
		return PAM_USER_UNKNOWN;
	if (service_policy(pamh->service) == POLICY_PERMIT)
		return PAM_SUCCESS;

	hash = lookup_hash(user, &unknown);
	if (unknown)
		return PAM_USER_UNKNOWN;
	if (hash && hash_is_locked(hash))
		rc = PAM_AUTH_ERR; /* account is locked */
	free(hash);
	return rc;
}

int pam_setcred(pam_handle_t *pamh, int flags)
{
	(void)flags;
	return pamh ? PAM_SUCCESS : PAM_SYSTEM_ERR;
}

/* Record the login: the user's uid into /proc/self/loginuid.  Only root may
 * write it, and only for itself; a caller that is not root starts no login
 * and has nothing to record.  A system without the file (no /proc) has no
 * logins to record either. */
static int record_login(const char *user)
{
	struct passwd *pw;
	char buf[16];
	int fd, n, ok;

	if (geteuid() != 0)
		return PAM_SUCCESS;
	pw = user ? getpwnam(user) : NULL;
	if (!pw)
		return PAM_SESSION_ERR;
	n = snprintf(buf, sizeof(buf), "%u", (unsigned)pw->pw_uid);
	fd = open(PAM_LOGINUID_PATH, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return errno == ENOENT ? PAM_SUCCESS : PAM_SESSION_ERR;
	ok = write(fd, buf, (size_t)n) == n;
	close(fd);
	return ok ? PAM_SUCCESS : PAM_SESSION_ERR;
}

int pam_open_session(pam_handle_t *pamh, int flags)
{
	(void)flags;
	if (!pamh)
		return PAM_SESSION_ERR;
	if (service_records_login(pamh->service))
		return record_login(pamh->user);
	return PAM_SUCCESS;
}

int pam_close_session(pam_handle_t *pamh, int flags)
{
	(void)flags;
	return pamh ? PAM_SUCCESS : PAM_SESSION_ERR;
}

int pam_chauthtok(pam_handle_t *pamh, int flags)
{
	/* Not implemented: writing /etc/shadow is passwd(1)'s job. */
	(void)flags;
	return pamh ? PAM_SUCCESS : PAM_SYSTEM_ERR;
}

const char *pam_strerror(pam_handle_t *pamh, int errnum)
{
	(void)pamh;
	switch (errnum) {
	case PAM_SUCCESS:          return "Success";
	case PAM_OPEN_ERR:         return "Failed to load module";
	case PAM_SYMBOL_ERR:       return "Symbol not found";
	case PAM_SERVICE_ERR:      return "Error in service module";
	case PAM_SYSTEM_ERR:       return "System error";
	case PAM_BUF_ERR:          return "Memory buffer error";
	case PAM_PERM_DENIED:      return "Permission denied";
	case PAM_AUTH_ERR:         return "Authentication failure";
	case PAM_CRED_INSUFFICIENT:return "Insufficient credentials";
	case PAM_AUTHINFO_UNAVAIL: return "Authentication info unavailable";
	case PAM_USER_UNKNOWN:     return "User not known";
	case PAM_MAXTRIES:         return "Maximum tries exceeded";
	case PAM_NEW_AUTHTOK_REQD: return "Authentication token is no longer valid; new one required";
	case PAM_ACCT_EXPIRED:     return "Account expired";
	case PAM_SESSION_ERR:      return "Session failure";
	case PAM_CRED_UNAVAIL:     return "Credentials unavailable";
	case PAM_CRED_EXPIRED:     return "Credentials expired";
	case PAM_CRED_ERR:         return "Failure setting credentials";
	case PAM_NO_MODULE_DATA:   return "No module specific data is present";
	case PAM_CONV_ERR:         return "Conversation error";
	case PAM_AUTHTOK_ERR:      return "Authentication token error";
	case PAM_AUTHTOK_RECOVERY_ERR: return "Authentication token recovery error";
	case PAM_AUTHTOK_LOCK_BUSY: return "Authentication token lock busy";
	case PAM_AUTHTOK_DISABLE_AGING: return "Authentication token aging disabled";
	case PAM_TRY_AGAIN:        return "Failed preliminary check by password service";
	case PAM_IGNORE:           return "The return value should be ignored by PAM dispatch";
	case PAM_ABORT:            return "Critical error - abort";
	case PAM_AUTHTOK_EXPIRED:  return "Authentication token expired";
	case PAM_MODULE_UNKNOWN:   return "Module is unknown";
	case PAM_BAD_ITEM:         return "Bad item passed to pam_*_item()";
	case PAM_CONV_AGAIN:       return "Conversation is waiting for event";
	case PAM_INCOMPLETE:       return "Application needs to call libpam again";
	default:                   return "Unknown PAM error";
	}
}

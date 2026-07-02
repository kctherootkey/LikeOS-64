/*
 * pam.c - minimal PAM for LikeOS (libpam.so).
 *
 * Single built-in "unix" policy: authentication verifies the password against
 * /etc/shadow via crypt(); account management rejects unknown or locked
 * accounts; sessions and credential establishment are no-ops.  No /etc/pam.d
 * configuration or loadable modules yet - keep it simple, extend later.
 */
#include <security/pam_appl.h>
#include <stdlib.h>
#include <string.h>
#include <shadow.h>
#include <pwd.h>
#include <crypt.h>

struct pam_handle {
	char *service;
	char *user;
	char *authtok;
	char *tty;
	char *rhost;
	char *ruser;
	char *user_prompt;
	const struct pam_conv *conv;
	int last_status;
};

static char *pam_strdup(const char *s)
{
	return s ? strdup(s) : NULL;
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
	h->conv = pam_conversation;
	h->last_status = PAM_SUCCESS;
	*pamh = h;
	return PAM_SUCCESS;
}

int pam_end(pam_handle_t *pamh, int pam_status)
{
	if (!pamh)
		return PAM_SYSTEM_ERR;
	free(pamh->service);
	free(pamh->user);
	if (pamh->authtok)
		free(pamh->authtok);
	free(pamh->tty);
	free(pamh->rhost);
	free(pamh->ruser);
	free(pamh->user_prompt);
	free(pamh);
	return pam_status;
}

int pam_set_item(pam_handle_t *pamh, int item_type, const void *item)
{
	char **slot;

	if (!pamh)
		return PAM_SYSTEM_ERR;
	switch (item_type) {
	case PAM_SERVICE:     slot = &pamh->service; break;
	case PAM_USER:        slot = &pamh->user; break;
	case PAM_TTY:         slot = &pamh->tty; break;
	case PAM_RHOST:       slot = &pamh->rhost; break;
	case PAM_RUSER:       slot = &pamh->ruser; break;
	case PAM_USER_PROMPT: slot = &pamh->user_prompt; break;
	case PAM_AUTHTOK:     slot = &pamh->authtok; break;
	case PAM_CONV:
		pamh->conv = (const struct pam_conv *)item;
		return PAM_SUCCESS;
	default:
		return PAM_BAD_ITEM;
	}
	free(*slot);
	*slot = pam_strdup((const char *)item);
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
	case PAM_CONV:        *item = pamh->conv; break;
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

int pam_authenticate(pam_handle_t *pamh, int flags)
{
	const char *user = NULL;
	char *hash, *pw = NULL;
	struct crypt_data cd;
	char *computed;
	int unknown, rc;

	(void)flags;
	if (!pamh)
		return PAM_SYSTEM_ERR;

	rc = pam_get_user(pamh, &user, NULL);
	if (rc != PAM_SUCCESS)
		return rc;

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
		free(pamh->authtok);
		pamh->authtok = pam_strdup(pw);
	}

	/* Scrub and free the plaintext password. */
	memset(pw, 0, strlen(pw));
	free(pw);
	free(hash);
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

int pam_open_session(pam_handle_t *pamh, int flags)
{
	(void)flags;
	return pamh ? PAM_SUCCESS : PAM_SESSION_ERR;
}

int pam_close_session(pam_handle_t *pamh, int flags)
{
	(void)flags;
	return pamh ? PAM_SUCCESS : PAM_SESSION_ERR;
}

int pam_chauthtok(pam_handle_t *pamh, int flags)
{
	/* Not implemented: writing /etc/shadow is a future extension. */
	(void)flags;
	return pamh ? PAM_SUCCESS : PAM_SYSTEM_ERR;
}

const char *pam_strerror(pam_handle_t *pamh, int errnum)
{
	(void)pamh;
	switch (errnum) {
	case PAM_SUCCESS:          return "Success";
	case PAM_SYSTEM_ERR:       return "System error";
	case PAM_BUF_ERR:          return "Memory buffer error";
	case PAM_PERM_DENIED:      return "Permission denied";
	case PAM_AUTH_ERR:         return "Authentication failure";
	case PAM_CRED_INSUFFICIENT:return "Insufficient credentials";
	case PAM_AUTHINFO_UNAVAIL: return "Authentication info unavailable";
	case PAM_USER_UNKNOWN:     return "User not known";
	case PAM_MAXTRIES:         return "Maximum tries exceeded";
	case PAM_ACCT_EXPIRED:     return "Account expired";
	case PAM_SESSION_ERR:      return "Session failure";
	case PAM_CONV_ERR:         return "Conversation error";
	case PAM_AUTHTOK_ERR:      return "Authentication token error";
	case PAM_ABORT:            return "Critical error - abort";
	default:                   return "Unknown PAM error";
	}
}

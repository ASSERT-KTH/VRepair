/*
 * libp11 PAM Login Module
 * Copyright (C) 2003 Mario Strasser <mast@gmx.net>,
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef PACKAGE
#define PACKAGE "pam_p11"
#endif

#include <syslog.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/crypto.h>
#include <libp11.h>
#include <regex.h>

/* openssl deprecated API emulation */
#ifndef HAVE_EVP_MD_CTX_NEW
#define EVP_MD_CTX_new()	EVP_MD_CTX_create()
#endif
#ifndef HAVE_EVP_MD_CTX_FREE
#define EVP_MD_CTX_free(ctx)	EVP_MD_CTX_destroy((ctx))
#endif
#ifndef HAVE_EVP_MD_CTX_RESET
#define EVP_MD_CTX_reset(ctx)	EVP_MD_CTX_cleanup((ctx))
#endif

#ifdef ENABLE_NLS
#include <libintl.h>
#include <locale.h>
#define _(string) gettext(string)
#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif
#else
#define _(string) string
#endif

/* We have to make this definitions before we include the pam header files! */
#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT
#define PAM_SM_SESSION
#define PAM_SM_PASSWORD
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#ifdef HAVE_SECURITY_PAM_EXT_H
#include <security/pam_ext.h>
#else
#define pam_syslog(handle, level, msg...) syslog(level, ## msg)
#endif

#ifndef HAVE_PAM_VPROMPT
static int pam_vprompt(pam_handle_t *pamh, int style, char **response,
		const char *fmt, va_list args)
{
	int r = PAM_CRED_INSUFFICIENT;
	const struct pam_conv *conv;
	struct pam_message msg;
	struct pam_response *resp = NULL;
	struct pam_message *(msgp[1]);

	char text[128];
	vsnprintf(text, sizeof text, fmt, args);

	msgp[0] = &msg;
	msg.msg_style = style;
	msg.msg = text;

	if (PAM_SUCCESS != pam_get_item(pamh, PAM_CONV, (const void **) &conv)
			|| NULL == conv || NULL == conv->conv
			|| conv->conv(1, (const struct pam_message **) msgp, &resp, conv->appdata_ptr)
			|| NULL == resp) {
		goto err;
	}
	if (NULL != response) {
		if (resp[0].resp) {
			*response = strdup(resp[0].resp);
			if (NULL == *response) {
				pam_syslog(pamh, LOG_CRIT, "strdup() failed: %s",
						strerror(errno));
				goto err;
			}
		} else {
			*response = NULL;
		}
	}

	r = PAM_SUCCESS;
err:
	if (resp) {
		OPENSSL_cleanse(&resp[0].resp, sizeof resp[0].resp);
		free(&resp[0]);
	}
	return r;
}
#endif

#ifndef PAM_EXTERN
#define PAM_EXTERN extern
#endif

int prompt(int flags, pam_handle_t *pamh, int style, char **response,
		const char *fmt, ...)
{
	int r;

	if (PAM_SILENT == (flags & PAM_SILENT)
			&& style != PAM_TEXT_INFO
			&& style != PAM_PROMPT_ECHO_OFF) {
		/* PAM_SILENT does not override the prompting of the user for passwords
		 * etc., it only stops informative messages from being generated. We
		 * use PAM_TEXT_INFO and PAM_PROMPT_ECHO_OFF exclusively for the
		 * password prompt. */
		r = PAM_SUCCESS;
	} else {
		va_list args;
		va_start (args, fmt);
		if (!response) {
			char *p = NULL;
			r = pam_vprompt(pamh, style, &p, fmt, args);
			free(p);
		} else {
			r = pam_vprompt(pamh, style, response, fmt, args);
		}
		va_end(args);
	}

	return r;
}

struct module_data {
	PKCS11_CTX *ctx;
	PKCS11_SLOT *slots;
	unsigned int nslots;
	int module_loaded;
};

#ifdef TEST
static struct module_data *global_module_data = NULL;
#endif

void module_data_cleanup(pam_handle_t *pamh, void *data, int error_status)
{
	struct module_data *module_data = data;
	if (module_data) {
		if (1 == module_data->module_loaded) {
			PKCS11_release_all_slots(module_data->ctx, module_data->slots, module_data->nslots);
			PKCS11_CTX_unload(module_data->ctx);
		}
		PKCS11_CTX_free(module_data->ctx);
		EVP_cleanup();
		ERR_free_strings();
		free(module_data);
	}
}

static int module_initialize(pam_handle_t * pamh,
		int flags, int argc, const char **argv,
		struct module_data **module_data)
{
	int r;
	struct module_data *data = calloc(1, sizeof *data);
	if (NULL == data) {
		pam_syslog(pamh, LOG_CRIT, "calloc() failed: %s",
				strerror(errno));
		r = PAM_BUF_ERR;
		goto err;
	}

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
#endif

	/* Initialize OpenSSL */
	OpenSSL_add_all_algorithms();
	ERR_load_crypto_strings();

	/* Load and initialize PKCS#11 module */
	data->ctx = PKCS11_CTX_new();
	if (0 == argc || NULL == data->ctx
			|| 0 != PKCS11_CTX_load(data->ctx, argv[0])) {
		pam_syslog(pamh, LOG_ALERT, "Loading PKCS#11 engine failed: %s\n",
				ERR_reason_error_string(ERR_get_error()));
		prompt(flags, pamh, PAM_ERROR_MSG , NULL, _("Error loading PKCS#11 module"));
		r = PAM_NO_MODULE_DATA;
		goto err;
	}
	data->module_loaded = 1;
	if (0 != PKCS11_enumerate_slots(data->ctx, &data->slots, &data->nslots)) {
		pam_syslog(pamh, LOG_ALERT, "Initializing PKCS#11 engine failed: %s\n",
				ERR_reason_error_string(ERR_get_error()));
		prompt(flags, pamh, PAM_ERROR_MSG , NULL, _("Error initializing PKCS#11 module"));
		r = PAM_AUTHINFO_UNAVAIL;
		goto err;
	}

#ifdef TEST
	/* pam_set_data() is reserved for actual modules. For testing this would
	 * return PAM_SYSTEM_ERR, so we're saving the module data in a static
	 * variable. */
	r = PAM_SUCCESS;
	global_module_data = data;
#else
	r = pam_set_data(pamh, PACKAGE, data, module_data_cleanup);
	if (PAM_SUCCESS != r) {
		goto err;
	}
#endif

	*module_data = data;
	data = NULL;

err:
	module_data_cleanup(pamh, data, r);

	return r;
}

static int module_refresh(pam_handle_t *pamh,
		int flags, int argc, const char **argv,
		const char **user, PKCS11_CTX **ctx,
		PKCS11_SLOT **slots, unsigned int *nslots,
		const char **pin_regex)
{
	int r;
	struct module_data *module_data;

	if (PAM_SUCCESS != pam_get_data(pamh, PACKAGE, (void *)&module_data)
			|| NULL == module_data) {
		r = module_initialize(pamh, flags, argc, argv, &module_data);
		if (PAM_SUCCESS != r) {
			goto err;
		}
	} else {
		/* refresh all known slots */
		PKCS11_release_all_slots(module_data->ctx,
				module_data->slots, module_data->nslots);
		module_data->slots = NULL;
		module_data->nslots = 0;
		if (0 != PKCS11_enumerate_slots(module_data->ctx,
					&module_data->slots, &module_data->nslots)) {
			pam_syslog(pamh, LOG_ALERT, "Initializing PKCS#11 engine failed: %s\n",
					ERR_reason_error_string(ERR_get_error()));
			prompt(flags, pamh, PAM_ERROR_MSG , NULL, _("Error initializing PKCS#11 module"));
			r = PAM_AUTHINFO_UNAVAIL;
			goto err;
		}
	}

	if (1 < argc) {
		*pin_regex = argv[1];
	} else {
#ifdef __APPLE__
		/* If multiple PAMs are allowed for macOS' login, then the captured
		 * password is used for all possible modules. To not block the token's
		 * PIN if the user enters his standard password, we're refusing to use
		 * anything that doesn't look like a PIN. */
		*pin_regex = "^[[:digit:]]*$";
#else
		*pin_regex = NULL;
#endif
	}

	r = pam_get_user(pamh, user, NULL);
	if (PAM_SUCCESS != r) {
		pam_syslog(pamh, LOG_ERR, "pam_get_user() failed %s",
				pam_strerror(pamh, r));
		r = PAM_USER_UNKNOWN;
		goto err;
	}

	*ctx = module_data->ctx;
	*nslots = module_data->nslots;
	*slots = module_data->slots;

err:
	return r;
}

extern int match_user_opensc(EVP_PKEY *authkey, const char *login);
extern int match_user_openssh(EVP_PKEY *authkey, const char *login);

static int key_login(pam_handle_t *pamh, int flags, PKCS11_SLOT *slot, const char *pin_regex)
{
	char *password = NULL;
	int ok;

	if (0 == slot->token->loginRequired
#ifdef HAVE_PKCS11_IS_LOGGED_IN
			|| (0 == PKCS11_is_logged_in(slot, 0, &ok)
				&& ok == 1)
#endif
	   ) {
		ok = 1;
		goto err;
	}
	ok = 0;

	/* try to get stored item */
	if (PAM_SUCCESS == pam_get_item(pamh, PAM_AUTHTOK, (void *)&password)
			&& NULL != password) {
		password = strdup(password);
		if (NULL == password) {
			pam_syslog(pamh, LOG_CRIT, "strdup() failed: %s",
					strerror(errno));
			goto err;
		}
	} else {
		const char *pin_info;

		if (slot->token->userPinFinalTry) {
			pin_info = _(" (last try)");
		} else {
			pin_info = "";
		}

		if (0 != slot->token->secureLogin) {
			prompt(flags, pamh, PAM_TEXT_INFO, NULL,
					_("Login on PIN pad with %s%s"),
					slot->token->label, pin_info);
		} else {
			/* ask the user for the password if variable text is set */
			if (PAM_SUCCESS != prompt(flags, pamh,
						PAM_PROMPT_ECHO_OFF, &password,
						_("Login with %s%s: "),
						slot->token->label, pin_info)) {
				goto err;
			}
		}
	}

	if (NULL != password && NULL != pin_regex && 0 < strlen(pin_regex)) {
		regex_t regex;
		int regex_compiled = 0;
		int result = 0;
		result = regcomp(&regex, pin_regex, REG_EXTENDED);
		if (0 == result) {
			regex_compiled = 1;
			result = regexec(&regex, password, 0, NULL, 0);
		}
		if (result) {
			char regex_error[256];
			regerror(result, &regex, regex_error, sizeof regex_error);
			pam_syslog(pamh, LOG_CRIT, "PIN regex didn't match: %s",
					regex_error);
			if (1 == regex_compiled) {
				regfree(&regex);
			}
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("Invalid PIN"));
			goto err;
		}
		regfree(&regex);
	}

	if (0 != PKCS11_login(slot, 0, password)) {
		if (slot->token->userPinLocked) {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PIN not verified; PIN locked"));
		} else if (slot->token->userPinFinalTry) {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PIN not verified; one try remaining"));
		} else {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PIN not verified"));
		}
		goto err;
	}

	pam_set_item(pamh, PAM_AUTHTOK, password);

	ok = 1;

err:
	if (NULL != password) {
		OPENSSL_cleanse(password, strlen(password));
		free(password);
	}

	return ok;
}

static int key_change_login(pam_handle_t *pamh, int flags, PKCS11_SLOT *slot, const char *pin_regex)
{
	char *old = NULL, *new = NULL, *retyped = NULL;
	int ok;

	if (0 == slot->token->loginRequired) {
		/* we can't change a PIN */
		ok = 0;
		goto err;
	}
	ok = 0;

	/* We need a R/W public session to change the PIN via PUK or
	 * a R/W user session to change the PIN via PIN */
	if (0 != PKCS11_open_session(slot, 1)
			|| (0 == slot->token->userPinLocked
				&& 1 != key_login(pamh, flags, slot, pin_regex))) {
		goto err;
	}

	/* prompt for new PIN (and PUK if needed) */
	if (0 != slot->token->secureLogin) {
		if (0 != slot->token->userPinLocked) {
			prompt(flags, pamh, PAM_TEXT_INFO, NULL,
					_("Change PIN with PUK on PIN pad for %s"),
					slot->token->label);
		} else {
			prompt(flags, pamh, PAM_TEXT_INFO, NULL,
					_("Change PIN on PIN pad for %s"),
					slot->token->label);
		}
	} else {
		if (0 != slot->token->userPinLocked) {
			if (PAM_SUCCESS == prompt(flags, pamh,
						PAM_PROMPT_ECHO_OFF, &old,
						_("PUK for %s: "),
						slot->token->label)) {
				goto err;
			}
		} else {
#ifdef TEST
			/* In the tests we're calling pam_sm_chauthtok() directly, so
			 * pam_get_item(PAM_AUTHTOK) will return PAM_BAD_ITEM. As
			 * workaround, we simply enter the current PIN twice. */
			if (PAM_SUCCESS != prompt(flags, pamh,
						PAM_PROMPT_ECHO_OFF, &old,
						_("Current PIN: "))) {
				goto err;
			}
#else
			if (PAM_SUCCESS != pam_get_item(pamh, PAM_AUTHTOK, (void *)&old)
					|| NULL == old) {
				goto err;
			}
			old = strdup(old);
			if (NULL == old) {
				pam_syslog(pamh, LOG_CRIT, "strdup() failed: %s",
						strerror(errno));
				goto err;
			}
#endif
		}
		if (PAM_SUCCESS != prompt(flags, pamh,
					PAM_PROMPT_ECHO_OFF, &new,
					_("Enter new PIN: "))
				|| PAM_SUCCESS != prompt(flags, pamh,
					PAM_PROMPT_ECHO_OFF, &retyped,
					_("Retype new PIN: "))) {
			goto err;
		}
		if (0 != strcmp(new, retyped)) {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PINs don't match"));
			goto err;
		}
	}

	if (0 != PKCS11_change_pin(slot, old, new)) {
		if (slot->token->userPinLocked) {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PIN not changed; PIN locked"));
		} else if (slot->token->userPinFinalTry) {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PIN not changed; one try remaining"));
		} else {
			prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("PIN not changed"));
		}
		goto err;
	}

	pam_set_item(pamh, PAM_AUTHTOK, new);

	ok = 1;

err:
	if (NULL != retyped) {
		OPENSSL_cleanse(retyped, strlen(retyped));
		free(retyped);
	}
	if (NULL != new) {
		OPENSSL_cleanse(new, strlen(new));
		free(new);
	}
	if (NULL != old) {
		OPENSSL_cleanse(old, strlen(old));
		free(old);
	}

	return ok;
}

static int key_find(pam_handle_t *pamh, int flags, const char *user,
		PKCS11_CTX *ctx, PKCS11_SLOT *slots, unsigned int nslots,
		PKCS11_SLOT **authslot, PKCS11_KEY **authkey)
{
	int token_found = 0;

	if (NULL == authslot || NULL == authkey) {
		return 0;
	}

	*authkey = NULL;
	*authslot = NULL;

	/* search all valuable slots for a key that is authorized by the user */
	while (0 < nslots) {
		PKCS11_SLOT *slot = NULL;
		PKCS11_CERT *certs = NULL;
#ifdef HAVE_PKCS11_ENUMERATE_PUBLIC_KEYS
		PKCS11_KEY *keys = NULL;
#endif
		unsigned int count = 0;

		slot = PKCS11_find_token(ctx, slots, nslots);
		if (NULL == slot || NULL == slot->token) {
			break;
		}
		token_found = 1;

		if (slot->token->loginRequired && slot->token->userPinLocked) {
			pam_syslog(pamh, LOG_DEBUG, "%s: PIN locked",
					slot->token->label);
			continue;
		}
		pam_syslog(pamh, LOG_DEBUG, "Searching %s for keys",
				slot->token->label);

#ifdef HAVE_PKCS11_ENUMERATE_PUBLIC_KEYS
		/* First, search all available public keys to allow using tokens
		 * without certificates (e.g. OpenPGP card) */
		if (0 == PKCS11_enumerate_public_keys(slot->token, &keys, &count)) {
			while (0 < count && NULL != keys) {
				EVP_PKEY *pubkey = PKCS11_get_public_key(keys);
				int r = match_user_opensc(pubkey, user);
				if (1 != r) {
					r = match_user_openssh(pubkey, user);
				}
				if (NULL != pubkey) {
					EVP_PKEY_free(pubkey);
				}
				if (1 == r) {
					*authkey = keys;
					*authslot = slot;
					pam_syslog(pamh, LOG_DEBUG, "Found %s",
							keys->label);
					return 1;
				}

				/* Try the next possible public key */
				keys++;
				count--;
			}
		}
#endif

		/* Next, search all certificates */
		if (0 == PKCS11_enumerate_certs(slot->token, &certs, &count)) {
			while (0 < count && NULL != certs) {
				EVP_PKEY *pubkey = X509_get_pubkey(certs->x509);
				int r = match_user_opensc(pubkey, user);
				if (1 != r) {
					r = match_user_openssh(pubkey, user);
				}
				if (NULL != pubkey) {
					EVP_PKEY_free(pubkey);
				}
				if (1 == r) {
					*authkey = PKCS11_find_key(certs);
					if (NULL == *authkey) {
						continue;
					}
					*authslot = slot;
					pam_syslog(pamh, LOG_DEBUG, "Found %s",
							certs->label);
					return 1;
				}

				/* Try the next possible certificate */
				certs++;
				count--;
			}
		}

		/* Try the next possible slot: PKCS11 slots are implemented as array,
		 * so starting to look at slot++ and decrementing nslots accordingly
		 * will search the rest of slots. */
		slot++;
		nslots -= (slot - slots);
		slots = slot;
		pam_syslog(pamh, LOG_DEBUG, "No authorized key found");
	}

	if (0 == token_found) {
		prompt(flags, pamh, PAM_ERROR_MSG , NULL, _("No token found"));
	} else {
		prompt(flags, pamh, PAM_ERROR_MSG , NULL, _("No authorized keys on token"));
	}

	return 0;
}

static int randomize(pam_handle_t *pamh, unsigned char *r, unsigned int r_len)
{
	int ok = 0;
	int fd = open("/dev/urandom", O_RDONLY);
	if (0 <= fd && read(fd, r, r_len) == (ssize_t)r_len) {
		ok = 1;
	} else {
		pam_syslog(pamh, LOG_CRIT, "Error reading from /dev/urandom: %s",
				strerror(errno));
	}
	if (0 <= fd) {
		close(fd);
	}
	return ok;
}

static int key_verify(pam_handle_t *pamh, int flags, PKCS11_KEY *authkey)
{
	int ok = 0;
	unsigned char challenge[30];
	unsigned char signature[256];
	unsigned int siglen = sizeof signature;
	const EVP_MD *md = EVP_sha1();
	EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
	EVP_PKEY *privkey = PKCS11_get_private_key(authkey);
	EVP_PKEY *pubkey = PKCS11_get_public_key(authkey);

	/* Verify a SHA-1 hash of random data, signed by the key.
	 *
	 * Note that this will not work keys that aren't eligible for signing.
	 * Unfortunately, libp11 currently has no way of checking
	 * C_GetAttributeValue(CKA_SIGN), see
	 * https://github.com/OpenSC/libp11/issues/219. Since we don't want to
	 * implement try and error, we live with this limitation */
	if (1 != randomize(pamh, challenge, sizeof challenge)) {
		goto err;
	}
	if (NULL == pubkey || NULL == privkey || NULL == md_ctx || NULL == md
			|| !EVP_SignInit(md_ctx, md)
			|| !EVP_SignUpdate(md_ctx, challenge, sizeof challenge)
			|| !EVP_SignFinal(md_ctx, signature, &siglen, privkey)
			|| !EVP_MD_CTX_reset(md_ctx)
			|| !EVP_VerifyInit(md_ctx, md)
			|| !EVP_VerifyUpdate(md_ctx, challenge, sizeof challenge)
			|| 1 != EVP_VerifyFinal(md_ctx, signature, siglen, pubkey)) {
		pam_syslog(pamh, LOG_DEBUG, "Error verifying key: %s\n",
				ERR_reason_error_string(ERR_get_error()));
		prompt(flags, pamh, PAM_ERROR_MSG, NULL, _("Error verifying key"));
		goto err;
	}
	ok = 1;

err:
	if (NULL != pubkey)
		EVP_PKEY_free(pubkey);
	if (NULL != privkey)
		EVP_PKEY_free(privkey);
	if (NULL != md_ctx) {
		EVP_MD_CTX_free(md_ctx);
	}
	return ok;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t * pamh, int flags, int argc,
		const char **argv)
{
	int r;
	PKCS11_CTX *ctx;
	unsigned int nslots;
	PKCS11_KEY *authkey;
	PKCS11_SLOT *slots, *authslot;
	const char *user;
	const char *pin_regex;

	r = module_refresh(pamh, flags, argc, argv,
			&user, &ctx, &slots, &nslots, &pin_regex);
	if (PAM_SUCCESS != r) {
		goto err;
	}

	if (1 != key_find(pamh, flags, user, ctx, slots, nslots,
				&authslot, &authkey)) {
		r = PAM_AUTHINFO_UNAVAIL;
		goto err;
	}
	if (1 != key_login(pamh, flags, authslot, pin_regex)
			|| 1 != key_verify(pamh, flags, authkey)) {
		if (authslot->token->userPinLocked) {
			r = PAM_MAXTRIES;
		} else {
			r = PAM_AUTH_ERR;
		}
		goto err;
	}

	r = PAM_SUCCESS;

err:
#ifdef TEST
	module_data_cleanup(pamh, global_module_data, r);
#endif
	return r;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t * pamh, int flags, int argc,
		const char **argv)
{
	/* Actually, we should return the same value as pam_sm_authenticate(). */
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t * pamh, int flags, int argc,
		const char **argv)
{
	/* if the user has been authenticated (precondition of this call), then
	 * everything is OK. Yes, we explicitly don't want to check CRLs, OCSP or
	 * exparation of certificates (use pam_pkcs11 for this). */
	return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t * pamh, int flags, int argc,
		const char **argv)
{
	pam_syslog(pamh, LOG_DEBUG,
			"Function pam_sm_open_session() is not implemented in this module");
	return PAM_SERVICE_ERR;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t * pamh, int flags, int argc,
		const char **argv)
{
	pam_syslog(pamh, LOG_DEBUG,
			"Function pam_sm_close_session() is not implemented in this module");
	return PAM_SERVICE_ERR;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t * pamh, int flags, int argc,
		const char **argv)
{
	int r;
	PKCS11_CTX *ctx;
	unsigned int nslots;
	PKCS11_KEY *authkey;
	PKCS11_SLOT *slots, *authslot;
	const char *user, *pin_regex;

	r = module_refresh(pamh, flags, argc, argv,
			&user, &ctx, &slots, &nslots, &pin_regex);
	if (PAM_SUCCESS != r) {
		goto err;
	}

	if (flags & PAM_CHANGE_EXPIRED_AUTHTOK) {
		/* Yes, we explicitly don't want to check CRLs, OCSP or exparation of
		 * certificates (use pam_pkcs11 for this). */
		r = PAM_SUCCESS;
		goto err;
	}

	if (1 != key_find(pamh, flags, user, ctx, slots, nslots,
				&authslot, &authkey)) {
		r = PAM_AUTHINFO_UNAVAIL;
		goto err;
	}

	if (flags & PAM_PRELIM_CHECK) {
		r = PAM_TRY_AGAIN;
		goto err;
	}

	if (flags & PAM_UPDATE_AUTHTOK) {
		if (1 != key_change_login(pamh, flags, authslot, pin_regex)) {
			if (authslot->token->userPinLocked) {
				r = PAM_MAXTRIES;
			} else {
				r = PAM_AUTH_ERR;
			}
			goto err;
		}
	}

	r = PAM_SUCCESS;

err:
#ifdef TEST
	module_data_cleanup(pamh, global_module_data, r);
#endif
	return r;
}

#ifdef PAM_STATIC
/* static module data */
struct pam_module _pam_group_modstruct = {
	PACKAGE,
	pam_sm_authenticate,
	pam_sm_setcred,
	pam_sm_acct_mgmt,
	pam_sm_open_session,
	pam_sm_close_session,
	pam_sm_chauthtok
};
#endif

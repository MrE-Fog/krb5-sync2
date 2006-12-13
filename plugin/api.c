/*
 * api.c
 *
 * The public APIs of the password update kadmind plugin.
 *
 * Provides the public pwupdate_init, pwupdate_close,
 * pwupdate_precommit_password, and pwupdate_postcommit_password APIs for the
 * kadmind plugin.  These APIs can also be called by command-line utilities.
 *
 * Active Directory synchronization is done in precommit and AFS kaserver
 * synchronization is done in postcommit.  The implication is that if Active
 * Directory synchronization fails, the update fails, but if AFS kaserver
 * synchronization fails, everything else still succeeds.
 */

#include <com_err.h>
#include <errno.h>
#include <krb5.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <plugin/internal.h>

/*
 * Load a string option from Kerberos appdefaults, setting the default to NULL
 * if the setting was not found.  This requires an annoying workaround because
 * one cannot specify a default value of NULL.
 */
static void
config_string(krb5_context ctx, const char *opt, char **result)
{
    const char *defval = "";

    krb5_appdefault_string(ctx, "krb5-sync", NULL, opt, defval, result);
    if (*result != NULL && (*result)[0] == '\0') {
        free(*result);
        *result = NULL;
    }
}

/*
 * Initialize the module.  This consists solely of loading our configuration
 * options from krb5.conf into a newly allocated struct stored in the second
 * argument to this function.  Returns 0 on success, non-zero on failre.  This
 * function returns failure only if it could not allocate memory.
 */
int
pwupdate_init(krb5_context ctx, void **data)
{
    struct plugin_config *config;

    config = malloc(sizeof(struct plugin_config));
    if (config == NULL)
        return 1;
    config_string(ctx, "afs_srvtab", &config->afs_srvtab);
    config_string(ctx, "afs_principal", &config->afs_principal);
    config_string(ctx, "afs_realm", &config->afs_realm);
    config_string(ctx, "ad_keytab", &config->ad_keytab);
    config_string(ctx, "ad_principal", &config->ad_principal);
    config_string(ctx, "ad_realm", &config->ad_realm);
    config_string(ctx, "ad_admin_server", &config->ad_admin_server);
    *data = config;
    return 0;
}

/*
 * Shut down the module.  This just means freeing our configuration struct,
 * since we don't store any other local state.
 */
void
pwupdate_close(void *data)
{
    struct plugin_config *config = data;

    if (config->afs_srvtab != NULL)
        free(config->afs_srvtab);
    if (config->afs_principal != NULL)
        free(config->afs_principal);
    if (config->afs_realm != NULL)
        free(config->afs_realm);
    if (config->ad_keytab != NULL)
        free(config->ad_keytab);
    if (config->ad_principal != NULL)
        free(config->ad_principal);
    if (config->ad_realm != NULL)
        free(config->ad_realm);
    if (config->ad_admin_server != NULL)
        free(config->ad_admin_server);
    free(config);
}

/*
 * Create a local Kerberos context and set the error appropriately if this
 * fails.  Return true on success, false otherwise.  Puts the error message in
 * errstr on failure.
 */
static int
create_context(krb5_context *ctx, char *errstr, int errstrlen)
{
    krb5_error_code ret;

    ret = krb5_init_context(ctx);
    if (ret != 0) {
        snprintf(errstr, errstrlen, "failure initializing Kerberos library:"
                 " %s", error_message(ret));
        return 1;
    }
    return 0;
}

/*
 * Check the principal for which we're changing a password.  If it contains a
 * non-null instance, we don't want to propagate the change; we only want to
 * change passwords for regular users.  Returns true if we should proceed,
 * false otherwise.  If we shouldn't proceed, logs a debug-level message to
 * syslog.
 */
static int
principal_allowed(krb5_context ctx, krb5_principal principal)
{
    if (krb5_princ_size(ctx, principal) > 1) {
        char *display;
        krb5_error_code ret;

        ret = krb5_unparse_name(ctx, principal, &display);
        if (ret != 0)
            display = NULL;
        syslog(LOG_DEBUG, "password synchronization skipping principal \"%s\""
               " with non-null instance", display != NULL ? display : "???");
        if (display != NULL)
            free(display);
        return 0;
    }
    return 1;
}

/*
 * Actions to take before the password is changed in the local database.
 *
 * Push the new password to Active Directory if we have the necessary
 * configuration information and return any error it returns, but skip any
 * principals with a non-NULL instance since those are kept separately in each
 * realm.
 */
int
pwupdate_precommit_password(void *data, krb5_principal principal,
                            char *password, int pwlen,
                            char *errstr, int errstrlen)
{
    struct plugin_config *config = data;
    krb5_context ctx;
    int status;

    if (config->ad_realm == NULL)
        return 0;
    if (!create_context(&ctx, errstr, errstrlen))
        return 1;
    if (!principal_allowed(ctx, principal))
        return 0;
    status = pwupdate_ad_change(config, ctx, principal, password, pwlen,
                                errstr, errstrlen);
    krb5_free_context(ctx);
    return status;
}

/*
 * Actions to take after the password is changed in the local database.
 *
 * Push the new password to the AFS kaserver if we have the necessary
 * configuration information and return any error it returns, but skip any
 * principals with a non-NULL instance since those are kept separately in each
 * realm.
 */
int pwupdate_postcommit_password(void *data, krb5_principal principal,
                                 char *password, int pwlen,
				 char *errstr, int errstrlen)
{
    struct plugin_config *config = data;
    krb5_context ctx;
    int status;

    if (config->afs_realm == NULL
        || config->afs_srvtab == NULL
        || config->afs_principal == NULL)
        return 0;
    if (!create_context(&ctx, errstr, errstrlen))
        return 1;
    if (!principal_allowed(ctx, principal))
        return 0;
    status = pwupdate_afs_change(config, ctx, principal, password, pwlen,
                                 errstr, errstrlen);
    krb5_free_context(ctx);
    return status;
}
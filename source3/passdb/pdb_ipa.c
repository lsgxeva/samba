/*
   Unix SMB/CIFS implementation.
   IPA helper functions for SAMBA
   Copyright (C) Sumit Bose <sbose@redhat.com> 2010

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "includes.h"
#include "libcli/security/dom_sid.h"

#include "smbldap.h"

#define LDAP_TRUST_CONTAINER "ou=system"
#define LDAP_ATTRIBUTE_CN "cn"
#define LDAP_ATTRIBUTE_TRUST_TYPE "sambaTrustType"
#define LDAP_ATTRIBUTE_TRUST_ATTRIBUTES "sambaTrustAttributes"
#define LDAP_ATTRIBUTE_TRUST_DIRECTION "sambaTrustDirection"
#define LDAP_ATTRIBUTE_TRUST_PARTNER "sambaTrustPartner"
#define LDAP_ATTRIBUTE_FLAT_NAME "sambaFlatName"
#define LDAP_ATTRIBUTE_TRUST_AUTH_OUTGOING "sambaTrustAuthOutgoing"
#define LDAP_ATTRIBUTE_TRUST_AUTH_INCOMING "sambaTrustAuthIncoming"
#define LDAP_ATTRIBUTE_SECURITY_IDENTIFIER "sambaSecurityIdentifier"
#define LDAP_ATTRIBUTE_TRUST_FOREST_TRUST_INFO "sambaTrustForestTrustInfo"

#define LDAP_OBJ_KRB_PRINCIPAL "krbPrincipal"
#define LDAP_OBJ_KRB_PRINCIPAL_AUX "krbPrincipalAux"
#define LDAP_ATTRIBUTE_KRB_PRINCIPAL "krbPrincipalName"

struct ipasam_privates {
	NTSTATUS (*ldapsam_add_sam_account)(struct pdb_methods *,
					    struct samu *sampass);
	NTSTATUS (*ldapsam_update_sam_account)(struct pdb_methods *,
					       struct samu *sampass);
};

static bool ipasam_get_trusteddom_pw(struct pdb_methods *methods,
				     const char *domain,
				     char** pwd,
				     struct dom_sid *sid,
				     time_t *pass_last_set_time)
{
	return false;
}

static bool ipasam_set_trusteddom_pw(struct pdb_methods *methods,
				     const char* domain,
				     const char* pwd,
				     const struct dom_sid *sid)
{
	return false;
}

static bool ipasam_del_trusteddom_pw(struct pdb_methods *methods,
				     const char *domain)
{
	return false;
}

static char *get_account_dn(const char *name)
{
	char *escape_name;
	char *dn;

	escape_name = escape_rdn_val_string_alloc(name);
	if (!escape_name) {
		return NULL;
	}

	if (name[strlen(name)-1] == '$') {
		dn = talloc_asprintf(talloc_tos(), "uid=%s,%s", escape_name,
				     lp_ldap_machine_suffix());
	} else {
		dn = talloc_asprintf(talloc_tos(), "uid=%s,%s", escape_name,
				     lp_ldap_user_suffix());
	}

	SAFE_FREE(escape_name);

	return dn;
}

static char *trusted_domain_dn(struct ldapsam_privates *ldap_state,
			       const char *domain)
{
	return talloc_asprintf(talloc_tos(), "%s=%s,%s,%s",
			       LDAP_ATTRIBUTE_CN, domain,
			       LDAP_TRUST_CONTAINER, ldap_state->domain_dn);
}

static char *trusted_domain_base_dn(struct ldapsam_privates *ldap_state)
{
	return talloc_asprintf(talloc_tos(), "%s,%s",
			       LDAP_TRUST_CONTAINER, ldap_state->domain_dn);
}

static bool get_trusted_domain_int(struct ldapsam_privates *ldap_state,
				   TALLOC_CTX *mem_ctx,
				   const char *filter, LDAPMessage **entry)
{
	int rc;
	char *base_dn = NULL;
	LDAPMessage *result = NULL;
	uint32_t num_result;

	base_dn = trusted_domain_base_dn(ldap_state);
	if (base_dn == NULL) {
		return false;
	}

	rc = smbldap_search(ldap_state->smbldap_state, base_dn,
			    LDAP_SCOPE_SUBTREE, filter, NULL, 0, &result);
	TALLOC_FREE(base_dn);

	if (result != NULL) {
		talloc_autofree_ldapmsg(mem_ctx, result);
	}

	if (rc == LDAP_NO_SUCH_OBJECT) {
		*entry = NULL;
		return true;
	}

	if (rc != LDAP_SUCCESS) {
		return false;
	}

	num_result = ldap_count_entries(priv2ld(ldap_state), result);

	if (num_result > 1) {
		DEBUG(1, ("get_trusted_domain_int: more than one "
			  "%s object with filter '%s'?!\n",
			  LDAP_OBJ_TRUSTED_DOMAIN, filter));
		return false;
	}

	if (num_result == 0) {
		DEBUG(1, ("get_trusted_domain_int: no "
			  "%s object with filter '%s'.\n",
			  LDAP_OBJ_TRUSTED_DOMAIN, filter));
		*entry = NULL;
	} else {
		*entry = ldap_first_entry(priv2ld(ldap_state), result);
	}

	return true;
}

static bool get_trusted_domain_by_name_int(struct ldapsam_privates *ldap_state,
					  TALLOC_CTX *mem_ctx,
					  const char *domain,
					  LDAPMessage **entry)
{
	char *filter = NULL;

	filter = talloc_asprintf(talloc_tos(),
				 "(&(objectClass=%s)(|(%s=%s)(%s=%s)(cn=%s)))",
				 LDAP_OBJ_TRUSTED_DOMAIN,
				 LDAP_ATTRIBUTE_FLAT_NAME, domain,
				 LDAP_ATTRIBUTE_TRUST_PARTNER, domain, domain);
	if (filter == NULL) {
		return false;
	}

	return get_trusted_domain_int(ldap_state, mem_ctx, filter, entry);
}

static bool get_trusted_domain_by_sid_int(struct ldapsam_privates *ldap_state,
					   TALLOC_CTX *mem_ctx,
					   const char *sid, LDAPMessage **entry)
{
	char *filter = NULL;

	filter = talloc_asprintf(talloc_tos(), "(&(objectClass=%s)(%s=%s))",
				 LDAP_OBJ_TRUSTED_DOMAIN,
				 LDAP_ATTRIBUTE_SECURITY_IDENTIFIER, sid);
	if (filter == NULL) {
		return false;
	}

	return get_trusted_domain_int(ldap_state, mem_ctx, filter, entry);
}

static bool get_uint32_t_from_ldap_msg(struct ldapsam_privates *ldap_state,
				       LDAPMessage *entry,
				       const char *attr,
				       uint32_t *val)
{
	char *dummy;
	long int l;
	char *endptr;

	dummy = smbldap_talloc_single_attribute(priv2ld(ldap_state), entry,
						attr, talloc_tos());
	if (dummy == NULL) {
		DEBUG(9, ("Attribute %s not present.\n", attr));
		*val = 0;
		return true;
	}

	l = strtoul(dummy, &endptr, 10);
	TALLOC_FREE(dummy);

	if (l < 0 || l > UINT32_MAX || *endptr != '\0') {
		return false;
	}

	*val = l;

	return true;
}

static void get_data_blob_from_ldap_msg(TALLOC_CTX *mem_ctx,
					struct ldapsam_privates *ldap_state,
					LDAPMessage *entry, const char *attr,
					DATA_BLOB *_blob)
{
	char *dummy;
	DATA_BLOB blob;

	dummy = smbldap_talloc_single_attribute(priv2ld(ldap_state), entry, attr,
						talloc_tos());
	if (dummy == NULL) {
		DEBUG(9, ("Attribute %s not present.\n", attr));
		ZERO_STRUCTP(_blob);
	} else {
		blob = base64_decode_data_blob(dummy);
		if (blob.length == 0) {
			ZERO_STRUCTP(_blob);
		} else {
			_blob->length = blob.length;
			_blob->data = talloc_steal(mem_ctx, blob.data);
		}
	}
	TALLOC_FREE(dummy);
}

static bool fill_pdb_trusted_domain(TALLOC_CTX *mem_ctx,
				    struct ldapsam_privates *ldap_state,
				    LDAPMessage *entry,
				    struct pdb_trusted_domain **_td)
{
	char *dummy;
	bool res;
	struct pdb_trusted_domain *td;

	if (entry == NULL) {
		return false;
	}

	td = talloc_zero(mem_ctx, struct pdb_trusted_domain);
	if (td == NULL) {
		return false;
	}

	/* All attributes are MAY */

	dummy = smbldap_talloc_single_attribute(priv2ld(ldap_state), entry,
						LDAP_ATTRIBUTE_SECURITY_IDENTIFIER,
						talloc_tos());
	if (dummy == NULL) {
		DEBUG(9, ("Attribute %s not present.\n",
			  LDAP_ATTRIBUTE_SECURITY_IDENTIFIER));
		ZERO_STRUCT(td->security_identifier);
	} else {
		res = string_to_sid(&td->security_identifier, dummy);
		TALLOC_FREE(dummy);
		if (!res) {
			return false;
		}
	}

	get_data_blob_from_ldap_msg(td, ldap_state, entry,
				    LDAP_ATTRIBUTE_TRUST_AUTH_INCOMING,
				    &td->trust_auth_incoming);

	get_data_blob_from_ldap_msg(td, ldap_state, entry,
				    LDAP_ATTRIBUTE_TRUST_AUTH_OUTGOING,
				    &td->trust_auth_outgoing);

	td->netbios_name = smbldap_talloc_single_attribute(priv2ld(ldap_state),
							   entry,
							   LDAP_ATTRIBUTE_FLAT_NAME,
							   td);
	if (td->netbios_name == NULL) {
		DEBUG(9, ("Attribute %s not present.\n",
			  LDAP_ATTRIBUTE_FLAT_NAME));
	}

	td->domain_name = smbldap_talloc_single_attribute(priv2ld(ldap_state),
							  entry,
							  LDAP_ATTRIBUTE_TRUST_PARTNER,
							  td);
	if (td->domain_name == NULL) {
		DEBUG(9, ("Attribute %s not present.\n",
			  LDAP_ATTRIBUTE_TRUST_PARTNER));
	}

	res = get_uint32_t_from_ldap_msg(ldap_state, entry,
					 LDAP_ATTRIBUTE_TRUST_DIRECTION,
					 &td->trust_direction);
	if (!res) {
		return false;
	}

	res = get_uint32_t_from_ldap_msg(ldap_state, entry,
					 LDAP_ATTRIBUTE_TRUST_ATTRIBUTES,
					 &td->trust_attributes);
	if (!res) {
		return false;
	}

	res = get_uint32_t_from_ldap_msg(ldap_state, entry,
					 LDAP_ATTRIBUTE_TRUST_TYPE,
					 &td->trust_type);
	if (!res) {
		return false;
	}

	get_data_blob_from_ldap_msg(td, ldap_state, entry,
				    LDAP_ATTRIBUTE_TRUST_FOREST_TRUST_INFO,
				    &td->trust_forest_trust_info);

	*_td = td;

	return true;
}

static NTSTATUS ipasam_get_trusted_domain(struct pdb_methods *methods,
					  TALLOC_CTX *mem_ctx,
					  const char *domain,
					  struct pdb_trusted_domain **td)
{
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	LDAPMessage *entry = NULL;

	DEBUG(10, ("ipasam_get_trusted_domain called for domain %s\n", domain));

	if (!get_trusted_domain_by_name_int(ldap_state, talloc_tos(), domain,
					    &entry)) {
		return NT_STATUS_UNSUCCESSFUL;
	}
	if (entry == NULL) {
		DEBUG(5, ("ipasam_get_trusted_domain: no such trusted domain: "
			  "%s\n", domain));
		return NT_STATUS_NO_SUCH_DOMAIN;
	}

	if (!fill_pdb_trusted_domain(mem_ctx, ldap_state, entry, td)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

static NTSTATUS ipasam_get_trusted_domain_by_sid(struct pdb_methods *methods,
						 TALLOC_CTX *mem_ctx,
						 struct dom_sid *sid,
						 struct pdb_trusted_domain **td)
{
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	LDAPMessage *entry = NULL;
	char *sid_str;

	sid_str = sid_string_tos(sid);

	DEBUG(10, ("ipasam_get_trusted_domain_by_sid called for sid %s\n",
		   sid_str));

	if (!get_trusted_domain_by_sid_int(ldap_state, talloc_tos(), sid_str,
					   &entry)) {
		return NT_STATUS_UNSUCCESSFUL;
	}
	if (entry == NULL) {
		DEBUG(5, ("ipasam_get_trusted_domain_by_sid: no trusted domain "
			  "with sid: %s\n", sid_str));
		return NT_STATUS_NO_SUCH_DOMAIN;
	}

	if (!fill_pdb_trusted_domain(mem_ctx, ldap_state, entry, td)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

static bool smbldap_make_mod_uint32_t(LDAP *ldap_struct, LDAPMessage *entry,
				      LDAPMod ***mods, const char *attribute,
				      const uint32_t val)
{
	char *dummy;

	dummy = talloc_asprintf(talloc_tos(), "%lu", (unsigned long) val);
	if (dummy == NULL) {
		return false;
	}
	smbldap_make_mod(ldap_struct, entry, mods, attribute, dummy);
	TALLOC_FREE(dummy);

	return true;
}

static bool smbldap_make_mod_blob(LDAP *ldap_struct, LDAPMessage *entry,
				  LDAPMod ***mods, const char *attribute,
				  DATA_BLOB blob)
{
	char *dummy;

	dummy = base64_encode_data_blob(talloc_tos(), blob);
	if (dummy == NULL) {
		return false;
	}

	smbldap_make_mod(ldap_struct, entry, mods, attribute, dummy);
	TALLOC_FREE(dummy);

	return true;
}

static NTSTATUS ipasam_set_trusted_domain(struct pdb_methods *methods,
					  const char* domain,
					  const struct pdb_trusted_domain *td)
{
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	LDAPMessage *entry = NULL;
	LDAPMod **mods;
	bool res;
	char *trusted_dn = NULL;
	int ret;

	DEBUG(10, ("ipasam_set_trusted_domain called for domain %s\n", domain));

	res = get_trusted_domain_by_name_int(ldap_state, talloc_tos(), domain,
					     &entry);
	if (!res) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	mods = NULL;
	smbldap_make_mod(priv2ld(ldap_state), entry, &mods, "objectClass",
			 LDAP_OBJ_TRUSTED_DOMAIN);

	if (td->netbios_name != NULL) {
		smbldap_make_mod(priv2ld(ldap_state), entry, &mods,
				 LDAP_ATTRIBUTE_FLAT_NAME,
				 td->netbios_name);
	}

	if (td->domain_name != NULL) {
		smbldap_make_mod(priv2ld(ldap_state), entry, &mods,
				 LDAP_ATTRIBUTE_TRUST_PARTNER,
				 td->domain_name);
	}

	if (!is_null_sid(&td->security_identifier)) {
		smbldap_make_mod(priv2ld(ldap_state), entry, &mods,
				 LDAP_ATTRIBUTE_SECURITY_IDENTIFIER,
				 sid_string_tos(&td->security_identifier));
	}

	if (td->trust_type != 0) {
		res = smbldap_make_mod_uint32_t(priv2ld(ldap_state), entry,
						&mods, LDAP_ATTRIBUTE_TRUST_TYPE,
						td->trust_type);
		if (!res) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	if (td->trust_attributes != 0) {
		res = smbldap_make_mod_uint32_t(priv2ld(ldap_state), entry,
						&mods,
						LDAP_ATTRIBUTE_TRUST_ATTRIBUTES,
						td->trust_attributes);
		if (!res) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	if (td->trust_direction != 0) {
		res = smbldap_make_mod_uint32_t(priv2ld(ldap_state), entry,
						&mods,
						LDAP_ATTRIBUTE_TRUST_DIRECTION,
						td->trust_direction);
		if (!res) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	if (td->trust_auth_outgoing.data != NULL) {
		res = smbldap_make_mod_blob(priv2ld(ldap_state), entry,
					    &mods,
					    LDAP_ATTRIBUTE_TRUST_AUTH_OUTGOING,
					    td->trust_auth_outgoing);
		if (!res) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	if (td->trust_auth_incoming.data != NULL) {
		res = smbldap_make_mod_blob(priv2ld(ldap_state), entry,
					    &mods,
					    LDAP_ATTRIBUTE_TRUST_AUTH_INCOMING,
					    td->trust_auth_incoming);
		if (!res) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	if (td->trust_forest_trust_info.data != NULL) {
		res = smbldap_make_mod_blob(priv2ld(ldap_state), entry,
					    &mods,
					    LDAP_ATTRIBUTE_TRUST_FOREST_TRUST_INFO,
					    td->trust_forest_trust_info);
		if (!res) {
			return NT_STATUS_UNSUCCESSFUL;
		}
	}

	talloc_autofree_ldapmod(talloc_tos(), mods);

	trusted_dn = trusted_domain_dn(ldap_state, domain);
	if (trusted_dn == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	if (entry == NULL) {
		ret = smbldap_add(ldap_state->smbldap_state, trusted_dn, mods);
	} else {
		ret = smbldap_modify(ldap_state->smbldap_state, trusted_dn, mods);
	}

	if (ret != LDAP_SUCCESS) {
		DEBUG(1, ("error writing trusted domain data!\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}
	return NT_STATUS_OK;
}

static NTSTATUS ipasam_del_trusted_domain(struct pdb_methods *methods,
					  const char *domain)
{
	int ret;
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	LDAPMessage *entry = NULL;
	const char *dn;

	if (!get_trusted_domain_by_name_int(ldap_state, talloc_tos(), domain,
					    &entry)) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (entry == NULL) {
		DEBUG(5, ("ipasam_del_trusted_domain: no such trusted domain: "
			  "%s\n", domain));
		return NT_STATUS_NO_SUCH_DOMAIN;
	}

	dn = smbldap_talloc_dn(talloc_tos(), priv2ld(ldap_state), entry);
	if (dn == NULL) {
		DEBUG(0,("ipasam_del_trusted_domain: Out of memory!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	ret = smbldap_delete(ldap_state->smbldap_state, dn);
	if (ret != LDAP_SUCCESS) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

static NTSTATUS ipasam_enum_trusted_domains(struct pdb_methods *methods,
					    TALLOC_CTX *mem_ctx,
					    uint32_t *num_domains,
					    struct pdb_trusted_domain ***domains)
{
	int rc;
	struct ldapsam_privates *ldap_state =
		(struct ldapsam_privates *)methods->private_data;
	char *base_dn = NULL;
	char *filter = NULL;
	int scope = LDAP_SCOPE_SUBTREE;
	LDAPMessage *result = NULL;
	LDAPMessage *entry = NULL;

	filter = talloc_asprintf(talloc_tos(), "(objectClass=%s)",
				 LDAP_OBJ_TRUSTED_DOMAIN);
	if (filter == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	base_dn = trusted_domain_base_dn(ldap_state);
	if (base_dn == NULL) {
		TALLOC_FREE(filter);
		return NT_STATUS_NO_MEMORY;
	}

	rc = smbldap_search(ldap_state->smbldap_state, base_dn, scope, filter,
			    NULL, 0, &result);
	TALLOC_FREE(filter);
	TALLOC_FREE(base_dn);

	if (result != NULL) {
		talloc_autofree_ldapmsg(mem_ctx, result);
	}

	if (rc == LDAP_NO_SUCH_OBJECT) {
		*num_domains = 0;
		*domains = NULL;
		return NT_STATUS_OK;
	}

	if (rc != LDAP_SUCCESS) {
		return NT_STATUS_UNSUCCESSFUL;
	}

	*num_domains = 0;
	if (!(*domains = TALLOC_ARRAY(mem_ctx, struct pdb_trusted_domain *, 1))) {
		DEBUG(1, ("talloc failed\n"));
		return NT_STATUS_NO_MEMORY;
	}

	for (entry = ldap_first_entry(priv2ld(ldap_state), result);
	     entry != NULL;
	     entry = ldap_next_entry(priv2ld(ldap_state), entry))
	{
		struct pdb_trusted_domain *dom_info;

		if (!fill_pdb_trusted_domain(*domains, ldap_state, entry,
					     &dom_info)) {
			return NT_STATUS_UNSUCCESSFUL;
		}

		ADD_TO_ARRAY(*domains, struct pdb_trusted_domain *, dom_info,
			     domains, num_domains);

		if (*domains == NULL) {
			DEBUG(1, ("talloc failed\n"));
			return NT_STATUS_NO_MEMORY;
		}
	}

	DEBUG(5, ("ipasam_enum_trusted_domains: got %d domains\n", *num_domains));
	return NT_STATUS_OK;
}

static NTSTATUS ipasam_enum_trusteddoms(struct pdb_methods *methods,
					TALLOC_CTX *mem_ctx,
					uint32_t *num_domains,
					struct trustdom_info ***domains)
{
	NTSTATUS status;
	struct pdb_trusted_domain **td;
	int i;

	status = ipasam_enum_trusted_domains(methods, talloc_tos(),
					     num_domains, &td);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (*num_domains == 0) {
		return NT_STATUS_OK;
	}

	if (!(*domains = TALLOC_ARRAY(mem_ctx, struct trustdom_info *,
				      *num_domains))) {
		DEBUG(1, ("talloc failed\n"));
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < *num_domains; i++) {
		struct trustdom_info *dom_info;

		dom_info = TALLOC_P(*domains, struct trustdom_info);
		if (dom_info == NULL) {
			DEBUG(1, ("talloc failed\n"));
			return NT_STATUS_NO_MEMORY;
		}

		dom_info->name = talloc_steal(mem_ctx, td[i]->netbios_name);
		sid_copy(&dom_info->sid, &td[i]->security_identifier);

		(*domains)[i] = dom_info;
	}

	return NT_STATUS_OK;
}

static uint32_t pdb_ipasam_capabilities(struct pdb_methods *methods)
{
	return PDB_CAP_STORE_RIDS | PDB_CAP_ADS | PDB_CAP_TRUSTED_DOMAINS_EX;
}

static struct pdb_domain_info *pdb_ipasam_get_domain_info(struct pdb_methods *pdb_methods,
							  TALLOC_CTX *mem_ctx)
{
	struct pdb_domain_info *info;
	NTSTATUS status;
	struct ldapsam_privates *ldap_state = (struct ldapsam_privates *)pdb_methods->private_data;

	info = talloc(mem_ctx, struct pdb_domain_info);
	if (info == NULL) {
		return NULL;
	}

	info->name = talloc_strdup(info, ldap_state->domain_name);
	if (info->name == NULL) {
		goto fail;
	}

	/* TODO: read dns_domain, dns_forest and guid from LDAP */
	info->dns_domain = talloc_strdup(info, lp_realm());
	if (info->dns_domain == NULL) {
		goto fail;
	}
	strlower_m(info->dns_domain);
	info->dns_forest = talloc_strdup(info, info->dns_domain);

	sid_copy(&info->sid, &ldap_state->domain_sid);

	status = GUID_from_string("testguid", &info->guid);

	return info;

fail:
	TALLOC_FREE(info);
	return NULL;
}

static NTSTATUS modify_ipa_password_exop(struct ldapsam_privates *ldap_state,
					 struct samu *sampass)
{
	int ret;
	BerElement *ber = NULL;
	struct berval *bv = NULL;
	char *retoid = NULL;
	struct berval *retdata = NULL;
	const char *password;
	char *dn;

	password = pdb_get_plaintext_passwd(sampass);
	if (password == NULL || *password == '\0') {
		return NT_STATUS_INVALID_PARAMETER;
	}

	dn = get_account_dn(pdb_get_username(sampass));
	if (dn == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	ber = ber_alloc_t( LBER_USE_DER );
	if (ber == NULL) {
		DEBUG(7, ("ber_alloc_t failed.\n"));
		return NT_STATUS_NO_MEMORY;
	}

	ret = ber_printf(ber, "{tsts}", LDAP_TAG_EXOP_MODIFY_PASSWD_ID, dn,
			 LDAP_TAG_EXOP_MODIFY_PASSWD_NEW, password);
	if (ret == -1) {
		DEBUG(7, ("ber_printf failed.\n"));
		ber_free(ber, 1);
		return NT_STATUS_UNSUCCESSFUL;
	}

	ret = ber_flatten(ber, &bv);
	ber_free(ber, 1);
	if (ret == -1) {
		DEBUG(1, ("ber_flatten failed.\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	ret = smbldap_extended_operation(ldap_state->smbldap_state,
					 LDAP_EXOP_MODIFY_PASSWD, bv, NULL,
					 NULL, &retoid, &retdata);
	ber_bvfree(bv);
	if (retdata) {
		ber_bvfree(retdata);
	}
	if (retoid) {
		ldap_memfree(retoid);
	}
	if (ret != LDAP_SUCCESS) {
		DEBUG(1, ("smbldap_extended_operation LDAP_EXOP_MODIFY_PASSWD failed.\n"));
		return NT_STATUS_UNSUCCESSFUL;
	}

	return NT_STATUS_OK;
}

static NTSTATUS ipasam_add_objectclasses(struct ldapsam_privates *ldap_state,
					 struct samu *sampass)
{
	char *dn;
	LDAPMod **mods = NULL;
	int ret;
	char *princ;
	const char *domain;
	char *domain_with_dot;

	dn = get_account_dn(pdb_get_username(sampass));
	if (dn == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	princ = talloc_asprintf(talloc_tos(), "%s@%s", pdb_get_username(sampass), lp_realm());
	if (princ == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	domain = pdb_get_domain(sampass);
	if (domain == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	domain_with_dot = talloc_asprintf(talloc_tos(), "%s.", domain);
	if (domain_with_dot == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"objectclass", LDAP_OBJ_KRB_PRINCIPAL);
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			LDAP_ATTRIBUTE_KRB_PRINCIPAL, princ);
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"objectclass", LDAP_OBJ_KRB_PRINCIPAL_AUX);
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"objectclass", "ipaHost");
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"fqdn", domain);
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"objectclass", "posixAccount");
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"cn", pdb_get_username(sampass));
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"gidNumber", "12345");
	smbldap_set_mod(&mods, LDAP_MOD_ADD,
			"homeDirectory", "/dev/null");
	smbldap_set_mod(&mods, LDAP_MOD_ADD, "uid", domain);
	smbldap_set_mod(&mods, LDAP_MOD_ADD, "uid", domain_with_dot);

	ret = smbldap_modify(ldap_state->smbldap_state, dn, mods);
	ldap_mods_free(mods, true);
	if (ret != LDAP_SUCCESS) {
		DEBUG(1, ("failed to modify/add user with uid = %s (dn = %s)\n",
			  pdb_get_username(sampass),dn));
		return NT_STATUS_LDAP(ret);
	}

	return NT_STATUS_OK;
}

static NTSTATUS pdb_ipasam_add_sam_account(struct pdb_methods *pdb_methods,
					   struct samu *sampass)
{
	NTSTATUS status;
	struct ldapsam_privates *ldap_state;

	ldap_state = (struct ldapsam_privates *)(pdb_methods->private_data);

	status =ldap_state->ipasam_privates->ldapsam_add_sam_account(pdb_methods,
								     sampass);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	status = ipasam_add_objectclasses(ldap_state, sampass);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (pdb_get_init_flags(sampass, PDB_PLAINTEXT_PW) == PDB_CHANGED) {
		status = modify_ipa_password_exop(ldap_state, sampass);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
	}

	return NT_STATUS_OK;
}

static NTSTATUS pdb_ipasam_update_sam_account(struct pdb_methods *pdb_methods,
					      struct samu *sampass)
{
	NTSTATUS status;
	struct ldapsam_privates *ldap_state;
	ldap_state = (struct ldapsam_privates *)(pdb_methods->private_data);

	status = ldap_state->ipasam_privates->ldapsam_update_sam_account(pdb_methods,
									 sampass);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (pdb_get_init_flags(sampass, PDB_PLAINTEXT_PW) == PDB_CHANGED) {
		status = modify_ipa_password_exop(ldap_state, sampass);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
	}

	return NT_STATUS_OK;
}

static NTSTATUS pdb_init_IPA_ldapsam(struct pdb_methods **pdb_method, const char *location)
{
	struct ldapsam_privates *ldap_state;
	NTSTATUS status;

	status = pdb_init_ldapsam(pdb_method, location);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	(*pdb_method)->name = "IPA_ldapsam";

	ldap_state = (struct ldapsam_privates *)((*pdb_method)->private_data);
	ldap_state->ipasam_privates = talloc_zero(ldap_state,
						  struct ipasam_privates);
	if (ldap_state->ipasam_privates == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	ldap_state->is_ipa_ldap = true;
	ldap_state->ipasam_privates->ldapsam_add_sam_account = (*pdb_method)->add_sam_account;
	ldap_state->ipasam_privates->ldapsam_update_sam_account = (*pdb_method)->update_sam_account;

	(*pdb_method)->add_sam_account = pdb_ipasam_add_sam_account;
	(*pdb_method)->update_sam_account = pdb_ipasam_update_sam_account;

	(*pdb_method)->capabilities = pdb_ipasam_capabilities;
	(*pdb_method)->get_domain_info = pdb_ipasam_get_domain_info;

	(*pdb_method)->get_trusteddom_pw = ipasam_get_trusteddom_pw;
	(*pdb_method)->set_trusteddom_pw = ipasam_set_trusteddom_pw;
	(*pdb_method)->del_trusteddom_pw = ipasam_del_trusteddom_pw;
	(*pdb_method)->enum_trusteddoms = ipasam_enum_trusteddoms;

	(*pdb_method)->get_trusted_domain = ipasam_get_trusted_domain;
	(*pdb_method)->get_trusted_domain_by_sid = ipasam_get_trusted_domain_by_sid;
	(*pdb_method)->set_trusted_domain = ipasam_set_trusted_domain;
	(*pdb_method)->del_trusted_domain = ipasam_del_trusted_domain;
	(*pdb_method)->enum_trusted_domains = ipasam_enum_trusted_domains;

	return NT_STATUS_OK;
}

NTSTATUS pdb_ipa_init(void)
{
	NTSTATUS nt_status;

	if (!NT_STATUS_IS_OK(nt_status = smb_register_passdb(PASSDB_INTERFACE_VERSION, "IPA_ldapsam", pdb_init_IPA_ldapsam)))
		return nt_status;

	return NT_STATUS_OK;
}
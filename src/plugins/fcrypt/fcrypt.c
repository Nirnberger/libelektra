/**
 * @file
 *
 * @brief filter plugin providing cryptographic operations
 *
 * @copyright BSD License (see doc/COPYING or http://www.libelektra.org)
 *
 */

#ifndef HAVE_KDBCONFIG
#include "kdbconfig.h"
#endif
#include "fcrypt.h"
#include <gpg.h>
#include <kdb.h>
#include <kdberrors.h>
#include <kdbtypes.h>
#include <stdlib.h>
#include <string.h>


/**
 * @brief Allocates a new string holding the name of the temporary file.
 * @param file holds the path to the original file
 * @returns an allocated string holding the name of the encrypted file. Must be freed by the caller.
 */
static char * getTemporaryFileName (const char * file)
{
	const size_t newFileAllocated = strlen (file) + 7;
	char * newFile = elektraMalloc (newFileAllocated);
	if (!newFile) return NULL;
	snprintf (newFile, newFileAllocated, "%sXXXXXX", file);
	mktemp (newFile);
	return newFile;
}

/**
 * @brief Read number of total GPG recipient keys from the plugin configuration.
 * @param config holds the plugin configuration
 * @returns the number of GPG recipient keys.
 */
static size_t getRecipientCount (KeySet * config)
{
	Key * k;
	size_t recipientCount = 0;
	Key * root = ksLookupByName (config, ELEKTRA_CRYPTO_PARAM_GPG_KEY, 0);

	if (!root) return 0;

	// toplevel
	if (strlen (keyString (root)) > 0)
	{
		recipientCount++;
	}

	ksRewind (config);
	while ((k = ksNext (config)) != 0)
	{
		if (keyIsBelow (k, root))
		{
			recipientCount++;
		}
	}
	return recipientCount;
}

/**
 * @brief establish the Elektra plugin contract and decrypt the file provded at parentKey using GPG.
 * @retval 1 on success
 * @retval -1 on failure
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, get) (Plugin * handle, KeySet * ks ELEKTRA_UNUSED, Key * parentKey)
{
	// Publish module configuration to Elektra (establish the contract)
	if (!strcmp (keyName (parentKey), "system/elektra/modules/" ELEKTRA_PLUGIN_NAME))
	{
		KeySet * moduleConfig = ksNew (30,
#include "contract.h"
					       KS_END);
		ksAppend (ks, moduleConfig);
		ksDel (moduleConfig);
		return 1;
	}

	// TODO encrypt if this is a `postgetstorage` call

	// regular decryption business
	KeySet * pluginConfig = elektraPluginGetConfig (handle);

	// decrypt the file with gpg - prepare argument vector for gpg call
	static const int argc = 8;
	char * argv[8] = { NULL, "--batch", "--yes", "-o", NULL, "-d", NULL, NULL };
	char * tmpFile = getTemporaryFileName (keyString (parentKey));
	if (!tmpFile)
	{
		ELEKTRA_SET_ERROR (87, parentKey, "Memory allocation failed");
		return -1;
	}

	argv[4] = tmpFile;
	// safely discarding const from keyString() return value
	argv[6] = (char *)keyString (parentKey);

	// NOTE the decryption process works like this:
	// gpg2 --batch --yes -o tmpfile -d configFile
	// mv tmpfile configFile

	int result = elektraCryptoGpgCall (pluginConfig, parentKey, NULL, argv, argc);

	if (result == 1)
	{
		// encryption successful, overwrite the original file with the encrypted data
		if (rename (tmpFile, keyString (parentKey)) != 0)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_FCRYPT_RENAME, parentKey, "Renaming file %s to %s failed.", tmpFile,
					    keyString (parentKey));
			result = -1;
		}
	}

	elektraFree (tmpFile);
	return result;
}

/**
 * @brief Encrypt the file provided at parentKey using GPG.
 * @retval 1 on success
 * @retval -1 on failure
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, set) (Plugin * handle, KeySet * ks ELEKTRA_UNUSED, Key * parentKey)
{
	KeySet * pluginConfig = elektraPluginGetConfig (handle);

	const size_t recipientCount = getRecipientCount (pluginConfig);
	if (recipientCount == 0)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_CRYPTO_CONFIG_FAULT, parentKey,
				    "Missing GPG key (specified as %s) in plugin configuration.", ELEKTRA_CRYPTO_PARAM_GPG_KEY);
		return -1;
	}

	char * tmpFile = getTemporaryFileName (keyString (parentKey));
	if (!tmpFile)
	{
		ELEKTRA_SET_ERROR (87, parentKey, "Memory allocation failed");
		return -1;
	}

	// prepare argument vector for gpg call
	int argc = 8 + (2 * recipientCount);
	kdb_unsigned_short_t i = 0;
	char * argv[argc];
	argv[i++] = NULL;
	argv[i++] = "--batch";
	argv[i++] = "-o";
	argv[i++] = tmpFile;
	argv[i++] = "--yes"; // overwrite files if they exist

	// add recipients
	Key * k;
	Key * root = ksLookupByName (pluginConfig, ELEKTRA_CRYPTO_PARAM_GPG_KEY, 0);

	// append root (gpg/key) as gpg recipient
	if (root && strlen (keyString (root)) > 0)
	{
		argv[i++] = "-r";
		// NOTE argv[] values will not be modified, so const can be discarded safely
		argv[i++] = (char *)keyString (root);
	}

	// append keys beneath root (crypto/key/#_) as gpg recipients
	ksRewind (pluginConfig);
	while ((k = ksNext (pluginConfig)) != 0)
	{
		if (keyIsBelow (k, root))
		{
			argv[i++] = "-r";
			// NOTE argv[] values will not be modified, so const can be discarded safely
			argv[i++] = (char *)keyString (k);
		}
	}

	// prepare rest of the argument vector
	argv[i++] = "-e";
	argv[i++] = (char *)keyString (parentKey);
	argv[i++] = NULL;

	// NOTE the encryption process works like this:
	// gpg2 --batch --yes -o encryptedFile -r keyID -e configFile
	// mv encryptedFile configFile

	int result = elektraCryptoGpgCall (pluginConfig, parentKey, NULL, argv, argc);

	if (result == 1)
	{
		// encryption successful, overwrite the original file with the encrypted data
		if (rename (tmpFile, keyString (parentKey)) != 0)
		{
			ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_FCRYPT_RENAME, parentKey, "Renaming file %s to %s failed.", tmpFile,
					    keyString (parentKey));
			result = -1;
		}
	}

	elektraFree (tmpFile);
	return result;
}

/**
 * @brief Checks for the existense of the master password, that is used for encryption and decryption.
 *
 * If the master password can not be found it will be generated randomly.
 * Then it will be encrypted and stored in conf.
 *
 * If the master password can be found, it will be decrypted temporarily in order to verify its correctness.
 * conf will not be modified in this case.
 *
 * An error might occur during the password generation, encryption and decryption.
 * The error will be appended to errorKey.
 *
 * @retval 0 no changes were made to the configuration
 * @retval 1 the master password has been appended to the configuration
 * @retval -1 an error occured. Check errorKey
 */
int ELEKTRA_PLUGIN_FUNCTION (ELEKTRA_PLUGIN_NAME, checkconf) (Key * errorKey, KeySet * conf)
{
	if (getRecipientCount (conf) == 0)
	{
		ELEKTRA_SET_ERRORF (ELEKTRA_ERROR_CRYPTO_CONFIG_FAULT, errorKey,
				    "Missing GPG key (specified as %s) in plugin configuration.", ELEKTRA_CRYPTO_PARAM_GPG_KEY);
		return -1;
	}
	return 0;
}

Plugin * ELEKTRA_PLUGIN_EXPORT (fcrypt)
{
	// clang-format off
	return elektraPluginExport(ELEKTRA_PLUGIN_NAME,
			ELEKTRA_PLUGIN_GET,   &ELEKTRA_PLUGIN_FUNCTION(ELEKTRA_PLUGIN_NAME, get),
			ELEKTRA_PLUGIN_SET,   &ELEKTRA_PLUGIN_FUNCTION(ELEKTRA_PLUGIN_NAME, set),
			ELEKTRA_PLUGIN_END);
}

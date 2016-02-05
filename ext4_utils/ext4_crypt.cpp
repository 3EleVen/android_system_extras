/*
 * Copyright (c) 2015 Google, Inc.
 */

#include "ext4_crypt.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <asm/ioctl.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <android-base/logging.h>

#define XATTR_NAME_ENCRYPTION_POLICY "encryption.policy"
#define EXT4_KEYREF_DELIMITER ((char)'.')

// ext4enc:TODO Include structure from somewhere sensible
// MUST be in sync with ext4_crypto.c in kernel
#define EXT4_KEY_DESCRIPTOR_SIZE 8
#define EXT4_KEY_DESCRIPTOR_SIZE_HEX 17

struct ext4_encryption_policy {
    char version;
    char contents_encryption_mode;
    char filenames_encryption_mode;
    char flags;
    char master_key_descriptor[EXT4_KEY_DESCRIPTOR_SIZE];
} __attribute__((__packed__));

#define EXT4_ENCRYPTION_MODE_AES_256_XTS    1
#define EXT4_ENCRYPTION_MODE_AES_256_CTS    4

// ext4enc:TODO Get value from somewhere sensible
#define EXT4_IOC_SET_ENCRYPTION_POLICY _IOR('f', 19, struct ext4_encryption_policy)
#define EXT4_IOC_GET_ENCRYPTION_POLICY _IOW('f', 21, struct ext4_encryption_policy)

#define HEX_LOOKUP "0123456789abcdef"

static void policy_to_hex(const char* policy, char* hex) {
    for (size_t i = 0, j = 0; i < EXT4_KEY_DESCRIPTOR_SIZE; i++) {
        hex[j++] = HEX_LOOKUP[(policy[i] & 0xF0) >> 4];
        hex[j++] = HEX_LOOKUP[policy[i] & 0x0F];
    }
    hex[EXT4_KEY_DESCRIPTOR_SIZE_HEX - 1] = '\0';
}

static int is_dir_empty(const char *dirname)
{
    int n = 0;
    struct dirent *d;
    DIR *dir;

    dir = opendir(dirname);
    while ((d = readdir(dir)) != NULL) {
        if (strcmp(d->d_name, "lost+found") == 0) {
            // Skip lost+found directory
        } else if (++n > 2) {
            break;
        }
    }
    closedir(dir);
    return n <= 2;
}

int e4crypt_policy_set(const char *directory, const char *policy, size_t policy_length) {
    if (policy_length != EXT4_KEY_DESCRIPTOR_SIZE) {
        LOG(ERROR) << "Policy wrong length: " << policy_length;
        return -1;
    }

    if (access(directory, W_OK)) {
        PLOG(ERROR) << "Failed to access directory " << directory;
        return -1;
    }

    int fd = open(directory, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open directory " << directory;
        return -1;
    }

    if (!is_dir_empty(directory)) {
        LOG(ERROR) << "Can only set policy on an empty directory " << directory;
        return -1;
    }

    ext4_encryption_policy eep;
    eep.version = 0;
    eep.contents_encryption_mode = EXT4_ENCRYPTION_MODE_AES_256_XTS;
    eep.filenames_encryption_mode = EXT4_ENCRYPTION_MODE_AES_256_CTS;
    eep.flags = 0;
    memcpy(eep.master_key_descriptor, policy, EXT4_KEY_DESCRIPTOR_SIZE);
    if (ioctl(fd, EXT4_IOC_SET_ENCRYPTION_POLICY, &eep)) {
        PLOG(ERROR) << "Failed to set encryption policy for " << directory;
        close(fd);
        return -1;
    } else {
        close(fd);
    }

    char policy_hex[EXT4_KEY_DESCRIPTOR_SIZE_HEX];
    policy_to_hex(policy, policy_hex);
    LOG(INFO) << "Policy for " << directory << " set to " << policy_hex;
    return 0;
}

int e4crypt_policy_get(const char *directory, char *policy, size_t policy_length) {
    if (policy_length != EXT4_KEY_DESCRIPTOR_SIZE) {
        LOG(ERROR) << "Policy wrong length: " << policy_length;
        return -1;
    }

    if (access(directory, W_OK)) {
        PLOG(ERROR) << "Failed to access directory " << directory;
        return -1;
    }

    int fd = open(directory, O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        PLOG(ERROR) << "Failed to open directory " << directory;
        return -1;
    }

    ext4_encryption_policy eep;
    memset(&eep, 0, sizeof(ext4_encryption_policy));

    if (ioctl(fd, EXT4_IOC_GET_ENCRYPTION_POLICY, &eep)) {
        PLOG(WARNING) << "Failed to get encryption policy for " << directory;
        close(fd);
        return -1;
    } else {
        close(fd);
    }

    if ((eep.version == 0)
            && (eep.contents_encryption_mode == EXT4_ENCRYPTION_MODE_AES_256_XTS)
            && (eep.filenames_encryption_mode == EXT4_ENCRYPTION_MODE_AES_256_CTS)
            && (eep.flags == 0)) {
        memcpy(policy, eep.master_key_descriptor, EXT4_KEY_DESCRIPTOR_SIZE);
        return 0;
    }

    LOG(ERROR) << "Failed to find matching encryption policy for " << directory;
    return -1;
}

int e4crypt_policy_ensure(const char *directory, const char *policy, size_t policy_length) {
    if (policy_length != EXT4_KEY_DESCRIPTOR_SIZE) {
        LOG(ERROR) << "Policy wrong length: " << policy_length;
        return -1;
    }

    char existing_policy[EXT4_KEY_DESCRIPTOR_SIZE];
    if (e4crypt_policy_get(directory, existing_policy, EXT4_KEY_DESCRIPTOR_SIZE) == 0) {
        char policy_hex[EXT4_KEY_DESCRIPTOR_SIZE_HEX];
        char existing_policy_hex[EXT4_KEY_DESCRIPTOR_SIZE_HEX];

        policy_to_hex(policy, policy_hex);
        policy_to_hex(existing_policy, existing_policy_hex);

        if (memcmp(policy, existing_policy, EXT4_KEY_DESCRIPTOR_SIZE) == 0) {
            LOG(INFO) << "Found policy " << existing_policy_hex << " at " << directory
                    << " which matches expected value";
            return 0;
        } else {
            LOG(ERROR) << "Found policy " << existing_policy_hex << " at " << directory
                    << " which doesn't match expected value " << policy_hex;
            return -1;
        }
    }

    return e4crypt_policy_set(directory, policy, policy_length);
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _KMEMDUMP_H
#define _KMEMDUMP_H

enum kmemdump_uid {
	KMEMDUMP_ID_START = 0,
	KMEMDUMP_ID_USER_START,
	KMEMDUMP_ID_USER_END,
	KMEMDUMP_ID_NO_ID,
};

#ifdef CONFIG_KMEMDUMP
/**
 * struct kmemdump_zone - region mark zone information
 * @id: unique id for this zone
 * @zone: pointer to the memory area for this zone
 * @size: size of the memory area of this zone
 */
struct kmemdump_zone {
	enum kmemdump_uid	id;
	void			*zone;
	size_t			size;
};

#define KMEMDUMP_BACKEND_MAX_NAME 128
/**
 * struct kmemdump_backend - region mark backend information
 * @name: the name of the backend
 * @register_region: callback to register region in the backend
 * @unregister_region: callback to unregister region in the backend
 */
struct kmemdump_backend {
	char name[KMEMDUMP_BACKEND_MAX_NAME];
	int (*register_region)(const struct kmemdump_backend *be,
			       enum kmemdump_uid uid, void *vaddr, size_t size);
	int (*unregister_region)(const struct kmemdump_backend *be,
				 enum kmemdump_uid uid);
};

int kmemdump_register_backend(const struct kmemdump_backend *backend);
void kmemdump_unregister_backend(const struct kmemdump_backend *backend);

int kmemdump_register_id(enum kmemdump_uid id, void *zone, size_t size);

#define kmemdump_register(...)						\
	kmemdump_register_id(KMEMDUMP_ID_NO_ID, __VA_ARGS__)		\

void kmemdump_unregister(enum kmemdump_uid id);
#else
static inline int kmemdump_register_id(enum kmemdump_uid uid, void *area,
				       size_t size)
{
	return 0;
}

#define kmemdump_register(...)

static inline void kmemdump_unregister(enum kmemdump_uid id)
{
}
#endif

#endif

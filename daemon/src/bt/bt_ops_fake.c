/* bt/bt_ops_fake.c -- see bt_ops_fake.h. */
#include "bt_ops_fake.h"

#include <stdio.h>
#include <string.h>

static int fake_connect_profile(void *ctx, const char *mac_address, const char *bt_adapter,
				 const char *uuid, int timeout_ms, char *err_buf, size_t err_buf_len)
{
	m4_bt_ops_fake_t *f = ctx;

	f->connect_profile_calls++;
	snprintf(f->last_mac_address, sizeof(f->last_mac_address), "%s",
		 mac_address != NULL ? mac_address : "");
	snprintf(f->last_bt_adapter, sizeof(f->last_bt_adapter), "%s",
		 bt_adapter != NULL ? bt_adapter : "");
	snprintf(f->last_uuid, sizeof(f->last_uuid), "%s", uuid != NULL ? uuid : "");
	f->last_timeout_ms = timeout_ms;

	if (err_buf != NULL && err_buf_len > 0)
		snprintf(err_buf, err_buf_len, "%s", f->result_message);
	return f->result;
}

void m4_bt_ops_fake_init(m4_bt_ops_fake_t *fake)
{
	memset(fake, 0, sizeof(*fake));
	fake->result = 0;
	fake->_ops.ctx = fake;
	fake->_ops.connect_profile = fake_connect_profile;
}

const struct m4_bt_ops *m4_bt_ops_fake(m4_bt_ops_fake_t *fake)
{
	fake->_ops.ctx = fake;
	fake->_ops.connect_profile = fake_connect_profile;
	return &fake->_ops;
}

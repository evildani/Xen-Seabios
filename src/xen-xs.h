

#ifndef _XEN_XS_H

void xenbus_setup(void);
int xenbus_send(u32 type, u32 len, char *data,
                       u32 *reply_len, char **reply_data);
void test_xenbus(void);

#endif


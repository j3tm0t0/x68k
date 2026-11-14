/* bsrmt_host_test.c 用: bsrmt.c が使う IOCS だけ (中身は bsrmt_host_test.c のにせの BlueSCSI) */
struct iocs_time { int sec; int day; };
struct iocs_inquiry { unsigned char unit, info, ver, reserve, size, buff[0]; };
int _iocs_s_writeext(int lba, int block_count, int target, int block_size_code, const void *data);
int _iocs_s_readext(int lba, int block_count, int target, int block_size_code, void *buf);
int _iocs_s_request(int read_size, int target, void *buf);
int _iocs_s_inquiry(int read_size, int target, struct iocs_inquiry *buf);
int _iocs_timeget(void);
int _iocs_bindateget(void);
struct iocs_time _iocs_ontime(void);

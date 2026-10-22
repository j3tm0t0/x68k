void m_burn(void *p, unsigned int n){ volatile char *c=p; while(n--) *c++=0; }

// c_fix.c routes stdio/exit logging through LOGLOG; the launcher has no log sink, so drop it.
void LOGLOG(const char* file, const int line, const char* data) {
	(void)file; (void)line; (void)data;
}

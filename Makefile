fix_ts: fix_ts.c
	$(CC) $< -o $@ -lavformat -lavcodec -lavutil

clean:
	$(RM) -f fix_ts

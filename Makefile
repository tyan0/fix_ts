tsfix: tsfix.c
	$(CC) $< -o $@ -lavformat -lavcodec -lavutil

clean:
	$(RM) -f tsfix

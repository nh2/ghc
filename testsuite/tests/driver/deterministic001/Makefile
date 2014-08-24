TOP=../../..
include $(TOP)/mk/boilerplate.mk
include $(TOP)/mk/test.mk

# Deterministic compilation tests

clean:
	rm -f *.o *.hi
	rm -f Main$(exeext) Main-before$(exeext)

# bug #7215

deterministic001: clean
	echo 'module Main where main = return ()' > Main.hs
	'$(TEST_HC)' --make -v0 -O -fforce-recomp Main.hs
	mv Main Main-before
	touch Main.hs
	sleep 1
	'$(TEST_HC)' --make -v0 -O -fforce-recomp Main.hs
	cmp Main Main-before

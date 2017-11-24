update-patches:
	mkdir -p $(CURDIR)/debian/patches
	rm -f $(CURDIR)/debian/patches/*.patch
	git format-patch -N -o $(CURDIR)/debian/patches debian-patches ^upstream
	for patch in $$(ls $(CURDIR)/debian/patches/*.patch) ; \
	do \
		lines=$$(cat $$patch | wc -l) ; \
		head -n $$(($$lines - 3)) $$patch > $${patch}.chomped ; \
			mv $${patch}.chomped $$patch ; \
	done

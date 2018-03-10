new_monitor: new_monitor.cpp
	g++ -std=c++11 -O2 -ggdb -o new_monitor new_monitor.cpp json11/json11.cpp -I/usr/include/mysql/ -lmysqlpp -L/usr/lib64/mysql/ -lmysqlclient

install: new_monitor ejs.min.js
	cp -f new_monitor /var/www/cgi/new_monitor_beta
	cp -f ejs.min.js /var/www/moodle/ejs.min.js
	cp -f new_monitor.html /var/www/moodle/new_monitor.html

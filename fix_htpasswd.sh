#!/bin/bash
htpasswd -bc /etc/nginx/.htpasswd_hunter admin 'mohammaD123$%'
cat /etc/nginx/.htpasswd_hunter

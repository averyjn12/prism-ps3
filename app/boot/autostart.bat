# Prism (PRSM00001) v0.33: opens Prism once after the console starts.
# Prism's lines in /dev_hdd0/onxmb.bat call this file the first time the XMB comes up after a start
# (after the lines above them, the PS2 ones included); Prism's lines in /dev_hdd0/autoexec.bat leave
# the marker that allows it. SETTINGS in Prism turns both on or off.
# The boot disc goes in only AFTER the sign-in: put in at the sign-in screen (v0.30-v0.32) it froze
# the console there (Bananafuzzbuddy, 11-23). The sign-in is looked for every second; prism_signed_in marks
# when it was seen, for Prism's timing line in prism_autostart.log. The XMB moves to the newly
# inserted disc by itself, and webMAN presses X on it. The disc's program leaves prism_boot_ran.
# Hold L1 as the XMB comes up to stay on the XMB that time.
del /dev_hdd0/tmp/prism_autostart
abort if L1
del /dev_hdd0/tmp/prism_boot_ran
del /dev_hdd0/tmp/prism_signed_in
:prism_wait_signin
if logout
wait 1
goto prism_wait_signin
end if
md /dev_hdd0/tmp/prism_signed_in
wait xmb
/mount.ps3/dev_hdd0/tmp/prism_boot
wait 4
abort if exist /dev_hdd0/tmp/prism_boot_ran
abort if ingame
/play.ps3?col=game&seg=seg_device

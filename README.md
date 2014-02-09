Taskbar implementation for Weston
================================

<strong>A taskbar for Weston 1.3.1/1.4.0

by Tarnyko <tarnyko@*.net></strong>


## Description

 <strong>Weston</strong> is the reference implementation of a <strong>Wayland</strong> compositor. Its shell (desktop-shell) currently lacks a taskbar implementation, allowing to show and hide application surfaces on demand.

 Here is a WIP implementation. Ideally, its code will be decoupled from desktop-shell and will be callable using <i>xdg_shell_set_minimized()</i>.

![weston-taskbar in action](http://www.tarnyko.net/repo/weston131-taskbar1.png)

[weston-taskbar video](http://www.youtube.com/watch?v=7Svrb3iGBAs) 

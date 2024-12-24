labnag(1)

# NAME

labnag - Show dialog with message and buttons

# SYNOPSIS

_labnag_ [options...]

# OPTIONS

*-b, --button* <text> <action>
	Create a button with the text _text_ that executes _action_ when pressed.
	If the environment variable _TERMINAL_ is set, _action_ will be run inside
	the terminal. Otherwise, it will fallback to running directly. Multiple
	buttons can be defined by providing the flag multiple times.

*-B, --button-no-terminal* <text> <action>
	Create a button with the text _text_ that executes _action_ when pressed.
	_action_ will be run directly instead of in a terminal. Multiple buttons
	can be defined by providing the flag multiple times.

*-z, --button-dismiss* <text> <action>
	Create a button with the text _text_ that executes _action_ when pressed,
	and dismisses labnag. If the environment variable _TERMINAL_ is set,
	_action_ will be run inside the terminal. Otherwise, it will fallback to
	running directly. Multiple buttons can be defined by providing the flag
	multiple times.

*-Z, --button-dismiss-no-terminal* <text> <action>
	Create a button with the text _text_ that executes _action_ when pressed,
	and dismisses labnag. _action_ will be run directly instead of in a
	terminal. Multiple buttons can be defined by providing the flag multiple
	times.

*-d, --debug*
	Enable debugging.

*-e, --edge* top|bottom
	Set the edge to use.

*-y, --layer* overlay|top|bottom|background
	Set the layer to use.

*-f, --font* <font>
	Set the font to use.

*-h, --help*
	Show help message and quit.

*-l, --detailed-message*
	Read a detailed message from stdin. A button to toggle details will be
	added. Details are shown in a scrollable multi-line text area.

*-L, --detailed-button* <text>
	Set the text for the button that toggles details. This has no effect if
	there is not a detailed message. The default is _Toggle details_.

*-m, --message* <msg>
	Set the message text.

*-o, --output* <output>
	Set the output to use. This should be the name of a _xdg\_output_.

*-s, --dismiss-button* <text>
	Sets the text for the dismiss nagbar button. The default is _X_.

*-t, --timeout*
	Set duration to close dialog. Default is 5 seconds.

*-v, --version*
	Show the version number and quit.

# APPEARANCE OPTIONS

*--background* <RRGGBB[AA]>
	Set the color of the background.

*--border* <RRGGBB[AA]>
	Set the color of the border.

*--border-bottom* <RRGGBB[AA]>
	Set the color of the bottom border.

*--button-background* <RRGGBB[AA]>
	Set the color for the background for buttons.

*--text* <RRGGBB[AA]>
	Set the text color.

*--button-text* <RRGGBB[AA]>
	Set the button text color.

*--border-bottom-size* <size>
	Set the thickness of the bottom border.

*--message-padding* <padding>
	Set the padding for the message.

*--details-background* <RRGGBB[AA]>
	Set the color for the background for details.

*--details-border-size* <size>
	Set the thickness for the details border.

*--button-border-size* <size>
	Set the thickness for the button border.

*--button-gap* <gap>
	Set the size of the gap between buttons.

*--button-dismiss-gap* <gap>
	Set the size of the gap between the dismiss button and another button.

*--button-margin-right* <margin>
	Set the margin from the right of the dismiss button to edge.

*--button-padding* <padding>
	Set the padding for the button text.


/* MatonOS is a single-locale image; PipeWire's diagnostic translations are
 * intentionally disabled because Android bionic does not ship libintl. */
#pragma once
#define gettext(message) (message)
#define dgettext(domain, message) (message)
#define ngettext(singular, plural, count) ((count) == 1 ? (singular) : (plural))
#define dngettext(domain, singular, plural, count) \
    ((count) == 1 ? (singular) : (plural))
#define bindtextdomain(domain, directory) (domain)
#define bind_textdomain_codeset(domain, codeset) (codeset)
#define textdomain(domain) (domain)

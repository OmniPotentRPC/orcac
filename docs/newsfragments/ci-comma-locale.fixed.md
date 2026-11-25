CI generates `de_DE.UTF-8` in the build and sanitizer jobs and sets
`ORCAC_REQUIRE_COMMA_LOCALE`, under which the `comma-locale` test fails
instead of skipping when no comma-decimal locale is installed or no
binary ran in it. Hosted runners ship no such locale, so the locale
coverage was skipped on every CI run.

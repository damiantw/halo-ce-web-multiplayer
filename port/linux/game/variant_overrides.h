/*
VARIANT_OVERRIDES.H

A dedicated server rotation entry's rule changes: the "+key=value" parts of
"<map>:<game type>+key=value+key=value", applied on top of the built-in game
type the entry names.
*/

#ifndef __VARIANT_OVERRIDES_H
#define __VARIANT_OVERRIDES_H

enum
{
	/* the "key=value+key=value" text an entry keeps */
	VARIANT_OVERRIDES_LENGTH = 256,
};

struct game_variant;

/* checks "key=value+key=value" against the game type the variant is (its
game_engine_index): TRUE, or FALSE with the reason written */
boolean variant_overrides_validate(struct game_variant const *variant, char const *text,
	char *reason, unsigned long reason_size);

/* applies the (validated) changes to the variant; FALSE if one could not be */
boolean variant_overrides_apply(struct game_variant *variant, char const *text);

#endif

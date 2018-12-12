/*
 * Copyright (C) 2009 Dan Carpenter.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see http://www.gnu.org/copyleft/gpl.txt
 */

#include "parse.h"
#include "smatch.h"
#include "smatch_extra.h"
#include "smatch_slist.h"

ALLOCATOR(data_info, "smatch extra data");
ALLOCATOR(data_range, "data range");
__DO_ALLOCATOR(struct data_range, sizeof(struct data_range), __alignof__(struct data_range),
			 "permanent ranges", perm_data_range);

char *show_rl(struct range_list *list)
{
	struct data_range *tmp;
	char full[256];
	int i = 0;

	full[0] = '\0';
	full[255] = '\0';
	FOR_EACH_PTR(list, tmp) {
		if (i++)
			strncat(full, ",", 254 - strlen(full));
		if (sval_cmp(tmp->min, tmp->max) == 0) {
			strncat(full, sval_to_str(tmp->min), 254 - strlen(full));
			continue;
		}
		strncat(full, sval_to_str(tmp->min), 254 - strlen(full));
		strncat(full, "-", 254 - strlen(full));
		strncat(full, sval_to_str(tmp->max), 254 - strlen(full));
	} END_FOR_EACH_PTR(tmp);
	return alloc_sname(full);
}

static int str_to_comparison_arg_helper(const char *str,
		struct expression *call, int *comparison,
		struct expression **arg, char **endp)
{
	int param;
	char *c = (char *)str;

	if (*c != '[')
		return 0;
	c++;

	if (*c == '<') {
		c++;
		if (*c == '=') {
			*comparison = SPECIAL_LTE;
			c++;
		} else {
			*comparison = '<';
		}
	} else if (*c == '=') {
		c++;
		c++;
		*comparison = SPECIAL_EQUAL;
	} else if (*c == '>') {
		c++;
		if (*c == '=') {
			*comparison = SPECIAL_GTE;
			c++;
		} else {
			*comparison = '>';
		}
	} else if (*c == '!') {
		c++;
		c++;
		*comparison = SPECIAL_NOTEQUAL;
	} else {
		return 0;
	}

	if (*c != '$')
		return 0;
	c++;

	param = strtoll(c, &c, 10);
	c++; /* skip the ']' character */
	if (endp)
		*endp = (char *)c;

	if (!call)
		return 0;
	*arg = get_argument_from_call_expr(call->args, param);
	if (!*arg)
		return 0;
	return 1;
}

int str_to_comparison_arg(const char *str, struct expression *call, int *comparison, struct expression **arg)
{
	while (1) {
		if (!*str)
			return 0;
		if (*str == '[')
			break;
		str++;
	}
	return str_to_comparison_arg_helper(str, call, comparison, arg, NULL);
}

static int get_val_from_key(int use_max, struct symbol *type, char *c, struct expression *call, char **endp, sval_t *sval)
{
	struct expression *arg;
	int comparison;
	sval_t ret, tmp;

	if (use_max)
		ret = sval_type_max(type);
	else
		ret = sval_type_min(type);

	if (!str_to_comparison_arg_helper(c, call, &comparison, &arg, endp)) {
		*sval = ret;
		return 0;
	}

	if (use_max && get_implied_max(arg, &tmp)) {
		ret = tmp;
		if (comparison == '<') {
			tmp.value = 1;
			ret = sval_binop(ret, '-', tmp);
		}
	}
	if (!use_max && get_implied_min(arg, &tmp)) {
		ret = tmp;
		if (comparison == '>') {
			tmp.value = 1;
			ret = sval_binop(ret, '+', tmp);
		}
	}

	*sval = ret;
	return 1;
}

static sval_t add_one(sval_t sval)
{
	sval.value++;
	return sval;
}

static sval_t sub_one(sval_t sval)
{
	sval.value--;
	return sval;
}

void filter_by_comparison(struct range_list **rl, int comparison, struct range_list *right)
{
	struct range_list *left_orig = *rl;
	struct range_list *right_orig = right;
	struct range_list *ret_rl = *rl;
	struct symbol *cast_type;
	sval_t min, max;

	cast_type = rl_type(left_orig);
	if (sval_type_max(rl_type(left_orig)).uvalue < sval_type_max(rl_type(right_orig)).uvalue)
		cast_type = rl_type(right_orig);
	if (sval_type_max(cast_type).uvalue < INT_MAX)
		cast_type = &int_ctype;

	min = sval_type_min(cast_type);
	max = sval_type_max(cast_type);
	left_orig = cast_rl(cast_type, left_orig);
	right_orig = cast_rl(cast_type, right_orig);

	switch (comparison) {
	case '<':
	case SPECIAL_UNSIGNED_LT:
		ret_rl = remove_range(left_orig, rl_max(right_orig), max);
		break;
	case SPECIAL_LTE:
	case SPECIAL_UNSIGNED_LTE:
		if (!sval_is_max(rl_max(right_orig)))
			ret_rl = remove_range(left_orig, add_one(rl_max(right_orig)), max);
		break;
	case SPECIAL_EQUAL:
		if (!sval_is_max(rl_max(right_orig)))
			ret_rl = remove_range(ret_rl, add_one(rl_max(right_orig)), max);
		if (!sval_is_min(rl_min(right_orig)))
			ret_rl = remove_range(ret_rl, min, sub_one(rl_min(right_orig)));
		break;
	case SPECIAL_GTE:
	case SPECIAL_UNSIGNED_GTE:
		if (!sval_is_min(rl_min(right_orig)))
			ret_rl = remove_range(left_orig, min, sub_one(rl_min(right_orig)));
		break;
	case '>':
	case SPECIAL_UNSIGNED_GT:
		ret_rl = remove_range(left_orig, min, rl_min(right_orig));
		break;
	case SPECIAL_NOTEQUAL:
		if (sval_cmp(rl_min(right_orig), rl_max(right_orig)) == 0)
			ret_rl = remove_range(left_orig, rl_min(right_orig), rl_min(right_orig));
		break;
	default:
		sm_msg("internal error: unhandled comparison %s", show_special(comparison));
		return;
	}

	*rl = cast_rl(rl_type(*rl), ret_rl);
}

static struct range_list *filter_by_comparison_call(char *c, struct expression *call, char **endp, struct range_list *start_rl)
{
	struct expression *arg;
	struct range_list *right_orig;
	int comparison;

	if (!str_to_comparison_arg_helper(c, call, &comparison, &arg, endp))
		return 0;

	if (!get_implied_rl(arg, &right_orig))
		return 0;

	filter_by_comparison(&start_rl, comparison, right_orig);
	return start_rl;
}

static sval_t parse_val(int use_max, struct expression *call, struct symbol *type, char *c, char **endp)
{
	char *start = c;
	sval_t ret;

	if (!strncmp(start, "max", 3)) {
		ret = sval_type_max(type);
		c += 3;
	} else if (!strncmp(start, "u64max", 6)) {
		ret = sval_type_val(type, ULLONG_MAX);
		c += 6;
	} else if (!strncmp(start, "s64max", 6)) {
		ret = sval_type_val(type, LLONG_MAX);
		c += 6;
	} else if (!strncmp(start, "u32max", 6)) {
		ret = sval_type_val(type, UINT_MAX);
		c += 6;
	} else if (!strncmp(start, "s32max", 6)) {
		ret = sval_type_val(type, INT_MAX);
		c += 6;
	} else if (!strncmp(start, "u16max", 6)) {
		ret = sval_type_val(type, USHRT_MAX);
		c += 6;
	} else if (!strncmp(start, "s16max", 6)) {
		ret = sval_type_val(type, SHRT_MAX);
		c += 6;
	} else if (!strncmp(start, "min", 3)) {
		ret = sval_type_min(type);
		c += 3;
	} else if (!strncmp(start, "s64min", 6)) {
		ret = sval_type_val(type, LLONG_MIN);
		c += 6;
	} else if (!strncmp(start, "s32min", 6)) {
		ret = sval_type_val(type, INT_MIN);
		c += 6;
	} else if (!strncmp(start, "s16min", 6)) {
		ret = sval_type_val(type, SHRT_MIN);
		c += 6;
	} else if (!strncmp(start, "long_min", 8)) {
		ret = sval_type_val(type, LONG_MIN);
		c += 8;
	} else if (!strncmp(start, "long_max", 8)) {
		ret = sval_type_val(type, LONG_MAX);
		c += 8;
	} else if (!strncmp(start, "ulong_max", 9)) {
		ret = sval_type_val(type, ULONG_MAX);
		c += 8;
	} else if (!strncmp(start, "ptr_max", 7)) {
		ret = sval_type_val(type, valid_ptr_max);
		c += 8;
	} else if (start[0] == '[') {
		/* this parses [==p0] comparisons */
		get_val_from_key(1, type, start, call, &c, &ret);
	} else {
		ret = sval_type_val(type, strtoll(start, &c, 10));
	}
	*endp = c;
	return ret;
}

static char *jump_to_call_math(char *value)
{
	char *c = value;

	while (*c && *c != '[')
		c++;

	if (!*c)
		return NULL;
	c++;
	if (*c == '<' || *c == '=' || *c == '>' || *c == '!')
		return NULL;

	return c;
}

static void str_to_rl_helper(struct expression *call, struct symbol *type, char *value, struct range_list **rl)
{
	struct range_list *math_rl;
	sval_t min, max;
	char *call_math;
	char *c;

	if (!type)
		type = &llong_ctype;
	*rl = NULL;

	if (strcmp(value, "empty") == 0)
		return;

	if (strncmp(value, "[==$", 4) == 0) {
		struct expression *arg;
		int comparison;

		if (!str_to_comparison_arg(value, call, &comparison, &arg))
			return;
		if (!get_implied_rl(arg, rl))
			return;
		goto cast;
	}

	min = sval_type_min(type);
	max = sval_type_max(type);
	c = value;
	while (*c != '\0' && *c != '[') {
		if (*c == '(')
			c++;
		min = parse_val(0, call, type, c, &c);
		max = min;
		if (*c == ')')
			c++;
		if (*c == '\0' || *c == '[') {
			add_range(rl, min, min);
			break;
		}
		if (*c == ',') {
			add_range(rl, min, min);
			c++;
			continue;
		}
		if (*c != '-') {
			sm_msg("debug XXX: trouble parsing %s c = %s", value, c);
			break;
		}
		c++;
		if (*c == '(')
			c++;
		max = parse_val(1, call, type, c, &c);
		add_range(rl, min, max);
		if (*c == ')')
			c++;
		if (*c == ',')
			c++;
	}

	if (*c == '\0')
		goto cast;

	call_math = jump_to_call_math(value);
	if (call_math && parse_call_math_rl(call, call_math, &math_rl)) {
		*rl = rl_intersection(*rl, math_rl);
		goto cast;
	}

	/*
	 * For now if we already tried to handle the call math and couldn't
	 * figure it out then bail.
	 */
	if (jump_to_call_math(c) == c + 1)
		goto cast;

	*rl = filter_by_comparison_call(c, call, &c, *rl);

cast:
	*rl = cast_rl(type, *rl);
}

void str_to_rl(struct symbol *type, char *value, struct range_list **rl)
{
	return str_to_rl_helper(NULL, type, value, rl);
}

void call_results_to_rl(struct expression *expr, struct symbol *type, char *value, struct range_list **rl)
{
	return str_to_rl_helper(strip_expr(expr), type, value, rl);
}

int is_whole_rl(struct range_list *rl)
{
	struct data_range *drange;

	if (ptr_list_empty(rl))
		return 0;
	drange = first_ptr_list((struct ptr_list *)rl);
	if (sval_is_min(drange->min) && sval_is_max(drange->max))
		return 1;
	return 0;
}

sval_t rl_min(struct range_list *rl)
{
	struct data_range *drange;
	sval_t ret;

	ret.type = &llong_ctype;
	ret.value = LLONG_MIN;
	if (ptr_list_empty(rl))
		return ret;
	drange = first_ptr_list((struct ptr_list *)rl);
	return drange->min;
}

sval_t rl_max(struct range_list *rl)
{
	struct data_range *drange;
	sval_t ret;

	ret.type = &llong_ctype;
	ret.value = LLONG_MAX;
	if (ptr_list_empty(rl))
		return ret;
	drange = last_ptr_list((struct ptr_list *)rl);
	return drange->max;
}

int rl_to_sval(struct range_list *rl, sval_t *sval)
{
	sval_t min, max;

	if (!rl)
		return 0;

	min = rl_min(rl);
	max = rl_max(rl);
	if (sval_cmp(min, max) != 0)
		return 0;
	*sval = min;
	return 1;
}

struct symbol *rl_type(struct range_list *rl)
{
	if (!rl)
		return NULL;
	return rl_min(rl).type;
}

static struct data_range *alloc_range_helper_sval(sval_t min, sval_t max, int perm)
{
	struct data_range *ret;

	if (perm)
		ret = __alloc_perm_data_range(0);
	else
		ret = __alloc_data_range(0);
	ret->min = min;
	ret->max = max;
	return ret;
}

struct data_range *alloc_range(sval_t min, sval_t max)
{
	return alloc_range_helper_sval(min, max, 0);
}

struct data_range *alloc_range_perm(sval_t min, sval_t max)
{
	return alloc_range_helper_sval(min, max, 1);
}

struct range_list *alloc_rl(sval_t min, sval_t max)
{
	struct range_list *rl = NULL;

	if (sval_cmp(min, max) > 0)
		return alloc_whole_rl(min.type);

	add_range(&rl, min, max);
	return rl;
}

struct range_list *alloc_whole_rl(struct symbol *type)
{
	if (!type || type_positive_bits(type) < 0)
		type = &llong_ctype;
	if (type->type == SYM_ARRAY)
		type = &ptr_ctype;

	return alloc_rl(sval_type_min(type), sval_type_max(type));
}

void add_range(struct range_list **list, sval_t min, sval_t max)
{
	struct data_range *tmp = NULL;
	struct data_range *new = NULL;
	int check_next = 0;

	/*
	 * FIXME:  This has a problem merging a range_list like: min-0,3-max
	 * with a range like 1-2.  You end up with min-2,3-max instead of
	 * just min-max.
	 */
	FOR_EACH_PTR(*list, tmp) {
		if (check_next) {
			/* Sometimes we overlap with more than one range
			   so we have to delete or modify the next range. */
			if (max.value + 1 == tmp->min.value) {
				/* join 2 ranges here */
				new->max = tmp->max;
				DELETE_CURRENT_PTR(tmp);
				return;
			}

			/* Doesn't overlap with the next one. */
			if (sval_cmp(max, tmp->min) < 0)
				return;
			/* Partially overlaps with the next one. */
			if (sval_cmp(max, tmp->max) < 0) {
				tmp->min.value = max.value + 1;
				return;
			}
			/* Completely overlaps with the next one. */
			if (sval_cmp(max, tmp->max) >= 0) {
				DELETE_CURRENT_PTR(tmp);
				/* there could be more ranges to delete */
				continue;
			}
		}
		if (!sval_is_max(max) && max.value + 1 == tmp->min.value) {
			/* join 2 ranges into a big range */
			new = alloc_range(min, tmp->max);
			REPLACE_CURRENT_PTR(tmp, new);
			return;
		}
		if (sval_cmp(max, tmp->min) < 0) { /* new range entirely below */
			new = alloc_range(min, max);
			INSERT_CURRENT(new, tmp);
			return;
		}
		if (sval_cmp(min, tmp->min) < 0) { /* new range partially below */
			if (sval_cmp(max, tmp->max) < 0)
				max = tmp->max;
			else
				check_next = 1;
			new = alloc_range(min, max);
			REPLACE_CURRENT_PTR(tmp, new);
			if (!check_next)
				return;
			continue;
		}
		if (sval_cmp(max, tmp->max) <= 0) /* new range already included */
			return;
		if (sval_cmp(min, tmp->max) <= 0) { /* new range partially above */
			min = tmp->min;
			new = alloc_range(min, max);
			REPLACE_CURRENT_PTR(tmp, new);
			check_next = 1;
			continue;
		}
		if (!sval_is_min(min) && min.value - 1 == tmp->max.value) {
			/* join 2 ranges into a big range */
			new = alloc_range(tmp->min, max);
			REPLACE_CURRENT_PTR(tmp, new);
			check_next = 1;
			continue;
		}
		/* the new range is entirely above the existing ranges */
	} END_FOR_EACH_PTR(tmp);
	if (check_next)
		return;
	new = alloc_range(min, max);
	add_ptr_list(list, new);
}

struct range_list *clone_rl(struct range_list *list)
{
	struct data_range *tmp;
	struct range_list *ret = NULL;

	FOR_EACH_PTR(list, tmp) {
		add_ptr_list(&ret, tmp);
	} END_FOR_EACH_PTR(tmp);
	return ret;
}

struct range_list *clone_rl_permanent(struct range_list *list)
{
	struct data_range *tmp;
	struct data_range *new;
	struct range_list *ret = NULL;

	FOR_EACH_PTR(list, tmp) {
		new = alloc_range_perm(tmp->min, tmp->max);
		add_ptr_list(&ret, new);
	} END_FOR_EACH_PTR(tmp);
	return ret;
}

struct range_list *rl_union(struct range_list *one, struct range_list *two)
{
	struct data_range *tmp;
	struct range_list *ret = NULL;

	FOR_EACH_PTR(one, tmp) {
		add_range(&ret, tmp->min, tmp->max);
	} END_FOR_EACH_PTR(tmp);
	FOR_EACH_PTR(two, tmp) {
		add_range(&ret, tmp->min, tmp->max);
	} END_FOR_EACH_PTR(tmp);
	return ret;
}

struct range_list *remove_range(struct range_list *list, sval_t min, sval_t max)
{
	struct data_range *tmp;
	struct range_list *ret = NULL;

	FOR_EACH_PTR(list, tmp) {
		if (sval_cmp(tmp->max, min) < 0) {
			add_range(&ret, tmp->min, tmp->max);
			continue;
		}
		if (sval_cmp(tmp->min, max) > 0) {
			add_range(&ret, tmp->min, tmp->max);
			continue;
		}
		if (sval_cmp(tmp->min, min) >= 0 && sval_cmp(tmp->max, max) <= 0)
			continue;
		if (sval_cmp(tmp->min, min) >= 0) {
			max.value++;
			add_range(&ret, max, tmp->max);
		} else if (sval_cmp(tmp->max, max) <= 0) {
			min.value--;
			add_range(&ret, tmp->min, min);
		} else {
			min.value--;
			max.value++;
			add_range(&ret, tmp->min, min);
			add_range(&ret, max, tmp->max);
		}
	} END_FOR_EACH_PTR(tmp);
	return ret;
}

int ranges_equiv(struct data_range *one, struct data_range *two)
{
	if (!one && !two)
		return 1;
	if (!one || !two)
		return 0;
	if (sval_cmp(one->min, two->min) != 0)
		return 0;
	if (sval_cmp(one->max, two->max) != 0)
		return 0;
	return 1;
}

int rl_equiv(struct range_list *one, struct range_list *two)
{
	struct data_range *one_range;
	struct data_range *two_range;

	if (one == two)
		return 1;

	PREPARE_PTR_LIST(one, one_range);
	PREPARE_PTR_LIST(two, two_range);
	for (;;) {
		if (!one_range && !two_range)
			return 1;
		if (!ranges_equiv(one_range, two_range))
			return 0;
		NEXT_PTR_LIST(one_range);
		NEXT_PTR_LIST(two_range);
	}
	FINISH_PTR_LIST(two_range);
	FINISH_PTR_LIST(one_range);

	return 1;
}

int true_comparison_range(struct data_range *left, int comparison, struct data_range *right)
{
	switch (comparison) {
	case '<':
	case SPECIAL_UNSIGNED_LT:
		if (sval_cmp(left->min, right->max) < 0)
			return 1;
		return 0;
	case SPECIAL_UNSIGNED_LTE:
	case SPECIAL_LTE:
		if (sval_cmp(left->min, right->max) <= 0)
			return 1;
		return 0;
	case SPECIAL_EQUAL:
		if (sval_cmp(left->max, right->min) < 0)
			return 0;
		if (sval_cmp(left->min, right->max) > 0)
			return 0;
		return 1;
	case SPECIAL_UNSIGNED_GTE:
	case SPECIAL_GTE:
		if (sval_cmp(left->max, right->min) >= 0)
			return 1;
		return 0;
	case '>':
	case SPECIAL_UNSIGNED_GT:
		if (sval_cmp(left->max, right->min) > 0)
			return 1;
		return 0;
	case SPECIAL_NOTEQUAL:
		if (sval_cmp(left->min, left->max) != 0)
			return 1;
		if (sval_cmp(right->min, right->max) != 0)
			return 1;
		if (sval_cmp(left->min, right->min) != 0)
			return 1;
		return 0;
	default:
		sm_msg("unhandled comparison %d\n", comparison);
		return 0;
	}
	return 0;
}

int true_comparison_range_LR(int comparison, struct data_range *var, struct data_range *val, int left)
{
	if (left)
		return true_comparison_range(var, comparison, val);
	else
		return true_comparison_range(val, comparison, var);
}

static int false_comparison_range_sval(struct data_range *left, int comparison, struct data_range *right)
{
	switch (comparison) {
	case '<':
	case SPECIAL_UNSIGNED_LT:
		if (sval_cmp(left->max, right->min) >= 0)
			return 1;
		return 0;
	case SPECIAL_UNSIGNED_LTE:
	case SPECIAL_LTE:
		if (sval_cmp(left->max, right->min) > 0)
			return 1;
		return 0;
	case SPECIAL_EQUAL:
		if (sval_cmp(left->min, left->max) != 0)
			return 1;
		if (sval_cmp(right->min, right->max) != 0)
			return 1;
		if (sval_cmp(left->min, right->min) != 0)
			return 1;
		return 0;
	case SPECIAL_UNSIGNED_GTE:
	case SPECIAL_GTE:
		if (sval_cmp(left->min, right->max) < 0)
			return 1;
		return 0;
	case '>':
	case SPECIAL_UNSIGNED_GT:
		if (sval_cmp(left->min, right->max) <= 0)
			return 1;
		return 0;
	case SPECIAL_NOTEQUAL:
		if (sval_cmp(left->max, right->min) < 0)
			return 0;
		if (sval_cmp(left->min, right->max) > 0)
			return 0;
		return 1;
	default:
		sm_msg("unhandled comparison %d\n", comparison);
		return 0;
	}
	return 0;
}

int false_comparison_range_LR(int comparison, struct data_range *var, struct data_range *val, int left)
{
	if (left)
		return false_comparison_range_sval(var, comparison, val);
	else
		return false_comparison_range_sval(val, comparison, var);
}

int possibly_true(struct expression *left, int comparison, struct expression *right)
{
	struct range_list *rl_left, *rl_right;
	struct data_range *tmp_left, *tmp_right;
	struct symbol *type;

	if (!get_implied_rl(left, &rl_left))
		return 1;
	if (!get_implied_rl(right, &rl_right))
		return 1;

	type = rl_type(rl_left);
	if (type_positive_bits(type) < type_positive_bits(rl_type(rl_right)))
		type = rl_type(rl_right);
	if (type_positive_bits(type) < 31)
		type = &int_ctype;

	rl_left = cast_rl(type, rl_left);
	rl_right = cast_rl(type, rl_right);

	FOR_EACH_PTR(rl_left, tmp_left) {
		FOR_EACH_PTR(rl_right, tmp_right) {
			if (true_comparison_range(tmp_left, comparison, tmp_right))
				return 1;
		} END_FOR_EACH_PTR(tmp_right);
	} END_FOR_EACH_PTR(tmp_left);
	return 0;
}

int possibly_false(struct expression *left, int comparison, struct expression *right)
{
	struct range_list *rl_left, *rl_right;
	struct data_range *tmp_left, *tmp_right;
	struct symbol *type;

	if (!get_implied_rl(left, &rl_left))
		return 1;
	if (!get_implied_rl(right, &rl_right))
		return 1;

	type = rl_type(rl_left);
	if (type_positive_bits(type) < type_positive_bits(rl_type(rl_right)))
		type = rl_type(rl_right);
	if (type_positive_bits(type) < 31)
		type = &int_ctype;

	rl_left = cast_rl(type, rl_left);
	rl_right = cast_rl(type, rl_right);

	FOR_EACH_PTR(rl_left, tmp_left) {
		FOR_EACH_PTR(rl_right, tmp_right) {
			if (false_comparison_range_sval(tmp_left, comparison, tmp_right))
				return 1;
		} END_FOR_EACH_PTR(tmp_right);
	} END_FOR_EACH_PTR(tmp_left);
	return 0;
}

int possibly_true_rl(struct range_list *left_ranges, int comparison, struct range_list *right_ranges)
{
	struct data_range *left_tmp, *right_tmp;

	if (!left_ranges || !right_ranges)
		return 1;

	FOR_EACH_PTR(left_ranges, left_tmp) {
		FOR_EACH_PTR(right_ranges, right_tmp) {
			if (true_comparison_range(left_tmp, comparison, right_tmp))
				return 1;
		} END_FOR_EACH_PTR(right_tmp);
	} END_FOR_EACH_PTR(left_tmp);
	return 0;
}

int possibly_false_rl(struct range_list *left_ranges, int comparison, struct range_list *right_ranges)
{
	struct data_range *left_tmp, *right_tmp;

	if (!left_ranges || !right_ranges)
		return 1;

	FOR_EACH_PTR(left_ranges, left_tmp) {
		FOR_EACH_PTR(right_ranges, right_tmp) {
			if (false_comparison_range_sval(left_tmp, comparison, right_tmp))
				return 1;
		} END_FOR_EACH_PTR(right_tmp);
	} END_FOR_EACH_PTR(left_tmp);
	return 0;
}

/* FIXME: the _rl here stands for right left so really it should be _lr */
int possibly_true_rl_LR(int comparison, struct range_list *a, struct range_list *b, int left)
{
	if (left)
		return possibly_true_rl(a, comparison, b);
	else
		return possibly_true_rl(b, comparison, a);
}

int possibly_false_rl_LR(int comparison, struct range_list *a, struct range_list *b, int left)
{
	if (left)
		return possibly_false_rl(a, comparison, b);
	else
		return possibly_false_rl(b, comparison, a);
}

int rl_has_sval(struct range_list *rl, sval_t sval)
{
	struct data_range *tmp;

	FOR_EACH_PTR(rl, tmp) {
		if (sval_cmp(tmp->min, sval) <= 0 &&
		    sval_cmp(tmp->max, sval) >= 0)
			return 1;
	} END_FOR_EACH_PTR(tmp);
	return 0;
}

void tack_on(struct range_list **list, struct data_range *drange)
{
	add_ptr_list(list, drange);
}

void push_rl(struct range_list_stack **rl_stack, struct range_list *rl)
{
	add_ptr_list(rl_stack, rl);
}

struct range_list *pop_rl(struct range_list_stack **rl_stack)
{
	struct range_list *rl;

	rl = last_ptr_list((struct ptr_list *)*rl_stack);
	delete_ptr_list_last((struct ptr_list **)rl_stack);
	return rl;
}

struct range_list *top_rl(struct range_list_stack *rl_stack)
{
	struct range_list *rl;

	rl = last_ptr_list((struct ptr_list *)rl_stack);
	return rl;
}

void filter_top_rl(struct range_list_stack **rl_stack, sval_t sval)
{
	struct range_list *rl;

	rl = pop_rl(rl_stack);
	rl = remove_range(rl, sval, sval);
	push_rl(rl_stack, rl);
}

static int sval_too_big(struct symbol *type, sval_t sval)
{
	if (type_bits(type) == 64)
		return 0;
	if (sval.uvalue > ((1ULL << type_bits(type)) - 1))
		return 1;
	return 0;
}

static void add_range_t(struct symbol *type, struct range_list **rl, sval_t min, sval_t max)
{
	/* If we're just adding a number, cast it and add it */
	if (sval_cmp(min, max) == 0) {
		add_range(rl, sval_cast(type, min), sval_cast(type, max));
		return;
	}

	/* If the range is within the type range then add it */
	if (sval_fits(type, min) && sval_fits(type, max)) {
		add_range(rl, sval_cast(type, min), sval_cast(type, max));
		return;
	}

	/*
	 * If the range we are adding has more bits than the range type then
	 * add the whole range type.  Eg:
	 * 0x8000000000000000 - 0xf000000000000000 -> cast to int
	 * This isn't totally the right thing to do.  We could be more granular.
	 */
	if (sval_too_big(type, min) || sval_too_big(type, max)) {
		add_range(rl, sval_type_min(type), sval_type_max(type));
		return;
	}

	/* Cast negative values to high positive values */
	if (sval_is_negative(min) && type_unsigned(type)) {
		if (sval_is_positive(max)) {
			if (sval_too_high(type, max)) {
				add_range(rl, sval_type_min(type), sval_type_max(type));
				return;
			}
			add_range(rl, sval_type_val(type, 0), sval_cast(type, max));
			max = sval_type_max(type);
		} else {
			max = sval_cast(type, max);
		}
		min = sval_cast(type, min);
		add_range(rl, min, max);
	}

	/* Cast high positive numbers to negative */
	if (sval_unsigned(max) && sval_is_negative(sval_cast(type, max))) {
		if (!sval_is_negative(sval_cast(type, min))) {
			add_range(rl, sval_cast(type, min), sval_type_max(type));
			min = sval_type_min(type);
		} else {
			min = sval_cast(type, min);
		}
		max = sval_cast(type, max);
		add_range(rl, min, max);
	}

	add_range(rl, min, max);
	return;
}

struct range_list *rl_truncate_cast(struct symbol *type, struct range_list *rl)
{
	struct data_range *tmp;
	struct range_list *ret = NULL;
	sval_t min, max;

	if (!rl)
		return NULL;

	if (!type || type == rl_type(rl))
		return rl;

	FOR_EACH_PTR(rl, tmp) {
		min = tmp->min;
		max = tmp->max;
		if (type_bits(type) < type_bits(rl_type(rl))) {
			min.uvalue = tmp->min.uvalue & ((1ULL << type_bits(type)) - 1);
			max.uvalue = tmp->max.uvalue & ((1ULL << type_bits(type)) - 1);
		}
		if (sval_cmp(min, max) > 0) {
			min = sval_cast(type, min);
			max = sval_cast(type, max);
		}
		add_range_t(type, &ret, min, max);
	} END_FOR_EACH_PTR(tmp);

	return ret;
}

static int rl_is_sane(struct range_list *rl)
{
	struct data_range *tmp;
	struct symbol *type;

	type = rl_type(rl);
	FOR_EACH_PTR(rl, tmp) {
		if (!sval_fits(type, tmp->min))
			return 0;
		if (!sval_fits(type, tmp->max))
			return 0;
		if (sval_cmp(tmp->min, tmp->max) > 0)
			return 0;
	} END_FOR_EACH_PTR(tmp);

	return 1;
}

static int rl_type_consistent(struct range_list *rl)
{
	struct data_range *tmp;
	struct symbol *type;

	type = rl_type(rl);
	FOR_EACH_PTR(rl, tmp) {
		if (type != tmp->min.type || type != tmp->max.type)
			return 0;
	} END_FOR_EACH_PTR(tmp);
	return 1;
}

struct range_list *cast_rl(struct symbol *type, struct range_list *rl)
{
	struct data_range *tmp;
	struct range_list *ret = NULL;

	if (!rl)
		return NULL;

	if (!type)
		return rl;
	if (!rl_is_sane(rl))
		return alloc_whole_rl(type);
	if (type == rl_type(rl) && rl_type_consistent(rl))
		return rl;

	FOR_EACH_PTR(rl, tmp) {
		add_range_t(type, &ret, tmp->min, tmp->max);
	} END_FOR_EACH_PTR(tmp);

	if (!ret)
		return alloc_whole_rl(type);

	return ret;
}

struct range_list *rl_invert(struct range_list *orig)
{
	struct range_list *ret = NULL;
	struct data_range *tmp;
	sval_t gap_min, abs_max, sval;

	if (!orig)
		return NULL;

	gap_min = sval_type_min(rl_min(orig).type);
	abs_max = sval_type_max(rl_max(orig).type);

	FOR_EACH_PTR(orig, tmp) {
		if (sval_cmp(tmp->min, gap_min) > 0) {
			sval = sval_type_val(tmp->min.type, tmp->min.value - 1);
			add_range(&ret, gap_min, sval);
		}
		gap_min = sval_type_val(tmp->max.type, tmp->max.value + 1);
		if (sval_cmp(tmp->max, abs_max) == 0)
			gap_min = abs_max;
	} END_FOR_EACH_PTR(tmp);

	if (sval_cmp(gap_min, abs_max) < 0)
		add_range(&ret, gap_min, abs_max);

	return ret;
}

struct range_list *rl_filter(struct range_list *rl, struct range_list *filter)
{
	struct data_range *tmp;

	FOR_EACH_PTR(filter, tmp) {
		rl = remove_range(rl, tmp->min, tmp->max);
	} END_FOR_EACH_PTR(tmp);

	return rl;
}

struct range_list *rl_intersection(struct range_list *one, struct range_list *two)
{
	struct range_list *one_orig;
	struct range_list *two_orig;
	struct range_list *ret;
	struct symbol *ret_type;
	struct symbol *small_type;
	struct symbol *large_type;

	if (!two)
		return NULL;
	if (!one)
		return NULL;

	one_orig = one;
	two_orig = two;

	ret_type = rl_type(one);
	small_type = rl_type(one);
	large_type = rl_type(two);

	if (type_bits(rl_type(two)) < type_bits(small_type)) {
		small_type = rl_type(two);
		large_type = rl_type(one);
	}

	one = cast_rl(large_type, one);
	two = cast_rl(large_type, two);

	ret = one;
	one = rl_invert(one);
	two = rl_invert(two);

	ret = rl_filter(ret, one);
	ret = rl_filter(ret, two);

	one = cast_rl(small_type, one_orig);
	two = cast_rl(small_type, two_orig);

	one = rl_invert(one);
	two = rl_invert(two);

	ret = cast_rl(small_type, ret);
	ret = rl_filter(ret, one);
	ret = rl_filter(ret, two);

	return cast_rl(ret_type, ret);
}

static struct range_list *handle_mod_rl(struct range_list *left, struct range_list *right)
{
	sval_t zero;
	sval_t max;

	max = rl_max(right);
	if (sval_is_max(max))
		return left;
	if (max.value == 0)
		return NULL;
	max.value--;
	if (sval_is_negative(max))
		return NULL;
	if (sval_cmp(rl_max(left), max) < 0)
		return left;
	zero = max;
	zero.value = 0;
	return alloc_rl(zero, max);
}

static struct range_list *handle_divide_rl(struct range_list *left, struct range_list *right)
{
	sval_t min, max;

	if (sval_is_max(rl_max(left)))
		return NULL;
	if (sval_is_max(rl_max(right)))
		return NULL;

	if (sval_is_negative(rl_min(left)))
		return NULL;
	if (sval_cmp_val(rl_min(right), 0) <= 0)
		return NULL;

	max = sval_binop(rl_max(left), '/', rl_min(right));
	min = sval_binop(rl_min(left), '/', rl_max(right));

	return alloc_rl(min, max);
}

static struct range_list *handle_add_mult_rl(struct range_list *left, int op, struct range_list *right)
{
	sval_t min, max;

	if (sval_binop_overflows(rl_min(left), op, rl_min(right)))
		return NULL;
	min = sval_binop(rl_min(left), op, rl_min(right));

	if (sval_binop_overflows(rl_max(left), op, rl_max(right)))
		return NULL;
	max = sval_binop(rl_max(left), op, rl_max(right));

	return alloc_rl(min, max);
}

static unsigned long long sval_fls_mask(sval_t sval)
{
	unsigned long long uvalue = sval.uvalue;
	unsigned long long high_bit = 0;

	while (uvalue) {
		uvalue >>= 1;
		high_bit++;
	}

	if (high_bit == 0)
		return 0;

	return ((unsigned long long)-1) >> (64 - high_bit);
}

static unsigned long long rl_bits_always_set(struct range_list *rl)
{
	return sval_fls_mask(rl_min(rl));
}

static unsigned long long rl_bits_maybe_set(struct range_list *rl)
{
	return sval_fls_mask(rl_max(rl));
}

static struct range_list *handle_OR_rl(struct range_list *left, struct range_list *right)
{
	unsigned long long left_min, left_max, right_min, right_max;
	sval_t min, max;
	sval_t sval;

	if ((rl_to_sval(left, &sval) || rl_to_sval(right, &sval)) &&
	    !sval_binop_overflows(rl_max(left), '+', rl_max(right)))
		return rl_binop(left, '+', right);

	left_min = rl_bits_always_set(left);
	left_max = rl_bits_maybe_set(left);
	right_min = rl_bits_always_set(right);
	right_max = rl_bits_maybe_set(right);

	min.type = max.type = &ullong_ctype;
	min.uvalue = left_min | right_min;
	max.uvalue = left_max | right_max;

	return cast_rl(rl_type(left), alloc_rl(min, max));
}

struct range_list *rl_binop(struct range_list *left, int op, struct range_list *right)
{
	struct symbol *cast_type;
	sval_t left_sval, right_sval;
	struct range_list *ret = NULL;

	cast_type = rl_type(left);
	if (sval_type_max(rl_type(left)).uvalue < sval_type_max(rl_type(right)).uvalue)
		cast_type = rl_type(right);
	if (sval_type_max(cast_type).uvalue < INT_MAX)
		cast_type = &int_ctype;

	left = cast_rl(cast_type, left);
	right = cast_rl(cast_type, right);

	if (!left || !right)
		return alloc_whole_rl(cast_type);

	if (rl_to_sval(left, &left_sval) && rl_to_sval(right, &right_sval)) {
		sval_t val = sval_binop(left_sval, op, right_sval);
		return alloc_rl(val, val);
	}

	switch (op) {
	case '%':
		ret = handle_mod_rl(left, right);
		break;
	case '/':
		ret = handle_divide_rl(left, right);
		break;
	case '*':
	case '+':
		ret = handle_add_mult_rl(left, op, right);
		break;
	case '|':
		ret = handle_OR_rl(left, right);
		break;

	/* FIXME:  Do the rest as well */
	case '-':
	case '&':
	case SPECIAL_RIGHTSHIFT:
	case SPECIAL_LEFTSHIFT:
	case '^':
		break;
	}

	if (!ret)
		ret = alloc_whole_rl(cast_type);
	return ret;
}

void free_rl(struct range_list **rlist)
{
	__free_ptr_list((struct ptr_list **)rlist);
}

static void free_single_dinfo(struct data_info *dinfo)
{
	free_rl(&dinfo->value_ranges);
}

static void free_dinfos(struct allocation_blob *blob)
{
	unsigned int size = sizeof(struct data_info);
	unsigned int offset = 0;

	while (offset < blob->offset) {
		free_single_dinfo((struct data_info *)(blob->data + offset));
		offset += size;
	}
}

void free_data_info_allocs(void)
{
	struct allocator_struct *desc = &data_info_allocator;
	struct allocation_blob *blob = desc->blobs;

	desc->blobs = NULL;
	desc->allocations = 0;
	desc->total_bytes = 0;
	desc->useful_bytes = 0;
	desc->freelist = NULL;
	while (blob) {
		struct allocation_blob *next = blob->next;
		free_dinfos(blob);
		blob_free(blob, desc->chunking);
		blob = next;
	}
	clear_data_range_alloc();
}


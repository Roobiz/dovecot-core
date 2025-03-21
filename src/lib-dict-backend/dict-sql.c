/* Copyright (c) 2005-2018 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "array.h"
#include "ioloop.h"
#include "istream.h"
#include "hex-binary.h"
#include "hash.h"
#include "str.h"
#include "sql-api-private.h"
#include "sql-db-cache.h"
#include "dict-private.h"
#include "dict-sql-settings.h"
#include "dict-sql.h"
#include "dict-sql-private.h"

#include <unistd.h>
#include <fcntl.h>

#define DICT_SQL_MAX_UNUSED_CONNECTIONS 10

enum sql_recurse_type {
	SQL_DICT_RECURSE_NONE,
	SQL_DICT_RECURSE_ONE,
	SQL_DICT_RECURSE_FULL
};

struct sql_dict_param {
	enum dict_sql_type value_type;

	const char *value_str;
	int64_t value_int64;
	double value_double;
	const void *value_binary;
	size_t value_binary_size;
	guid_128_t value_uuid;
};
ARRAY_DEFINE_TYPE(sql_dict_param, struct sql_dict_param);

struct sql_dict_iterate_context {
	struct dict_iterate_context ctx;
	pool_t pool;

	enum dict_iterate_flags flags;
	const char *path;

	struct sql_result *result;
	string_t *key;
	pool_t value_pool;
	const struct dict_sql_map *map;
	size_t key_prefix_len, pattern_prefix_len;
	unsigned int sql_fields_start_idx, next_map_idx;
	bool destroyed;
	bool synchronous_result;
	bool iter_query_sent;
	bool allow_null_map; /* allow next map to be NULL */
	const char *error;
};

struct sql_dict_inc_row {
	struct sql_dict_inc_row *prev;
	unsigned int rows;
};

struct sql_dict_prev {
	const struct dict_sql_map *map;
	char *key;
	union {
		char *str;
		long long diff;
	} value;
};

struct sql_dict_transaction_context {
	struct dict_transaction_context ctx;

	struct sql_transaction_context *sql_ctx;

	pool_t inc_row_pool;
	struct sql_dict_inc_row *inc_row;

	ARRAY(struct sql_dict_prev) prev_inc;
	ARRAY(struct sql_dict_prev) prev_set;

	dict_transaction_commit_callback_t *async_callback;
	void *async_context;

	char *error;
};

static struct sql_db_cache *dict_sql_db_cache;

static void sql_dict_prev_inc_flush(struct sql_dict_transaction_context *ctx);
static void sql_dict_prev_set_flush(struct sql_dict_transaction_context *ctx);
static void sql_dict_prev_inc_free(struct sql_dict_transaction_context *ctx);
static void sql_dict_prev_set_free(struct sql_dict_transaction_context *ctx);

static int
sql_dict_init(struct dict *driver, const char *uri,
	      const struct dict_settings *set,
	      struct dict **dict_r, const char **error_r)
{
	struct sql_settings sql_set;
	struct sql_dict *dict;
	pool_t pool;

	pool = pool_alloconly_create("sql dict", 2048);
	dict = p_new(pool, struct sql_dict, 1);
	dict->pool = pool;
	dict->dict = *driver;
	dict->set = dict_sql_settings_read(uri, error_r);
	if (dict->set == NULL) {
		pool_unref(&pool);
		return -1;
	}
	i_zero(&sql_set);
	sql_set.driver = driver->name;
	sql_set.connect_string = dict->set->connect;
	sql_set.event_parent = set->event_parent;

	if (sql_db_cache_new(dict_sql_db_cache, &sql_set, &dict->db, error_r) < 0) {
		pool_unref(&pool);
		return -1;
	}

	*dict_r = &dict->dict;
	return 0;
}

static void sql_dict_deinit(struct dict *_dict)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;

	sql_unref(&dict->db);
	pool_unref(&dict->pool);
}

static void sql_dict_wait(struct dict *_dict)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	sql_wait(dict->db);
}

/* Try to match path to map->pattern. For example pattern="shared/x/$/$/y"
   and path="shared/x/1/2/y", this is match and pattern_values=[1, 2]. */
static bool
dict_sql_map_match(const struct dict_sql_map *map, const char *path,
		   ARRAY_TYPE(const_string) *pattern_values, size_t *pat_len_r,
		   size_t *path_len_r, bool partial_ok, bool recurse)
{
	const char *path_start = path;
	const char *pat, *field, *p;
	size_t len;

	array_clear(pattern_values);
	pat = map->pattern;
	while (*pat != '\0' && *path != '\0') {
		if (*pat == '$') {
			/* variable */
			pat++;
			if (*pat == '\0') {
				/* pattern ended with this variable,
				   it'll match the rest of the path */
				len = strlen(path);
				if (partial_ok) {
					/* iterating - the last field never
					   matches fully. if there's a trailing
					   '/', drop it. */
					pat--;
					if (path[len-1] == '/') {
						field = t_strndup(path, len-1);
						array_push_back(pattern_values,
								&field);
					} else {
						array_push_back(pattern_values,
								&path);
					}
				} else {
					array_push_back(pattern_values, &path);
					path += len;
				}
				*path_len_r = path - path_start;
				*pat_len_r = pat - map->pattern;
				return TRUE;
			}
			/* pattern matches until the next '/' in path */
			p = strchr(path, '/');
			if (p != NULL) {
				field = t_strdup_until(path, p);
				array_push_back(pattern_values, &field);
				path = p;
			} else {
				/* no '/' anymore, but it'll still match a
				   partial */
				array_push_back(pattern_values, &path);
				path += strlen(path);
				pat++;
			}
		} else if (*pat == *path) {
			pat++;
			path++;
		} else {
			return FALSE;
		}
	}

	*path_len_r = path - path_start;
	*pat_len_r = pat - map->pattern;

	if (*pat == '\0')
		return *path == '\0';
	else if (!partial_ok)
		return FALSE;
	else {
		/* partial matches must end with '/'. */
		if (pat != map->pattern && pat[-1] != '/')
			return FALSE;
		/* if we're not recursing, there should be only one $variable
		   left. */
		if (recurse)
			return TRUE;
		return pat[0] == '$' && strchr(pat, '/') == NULL;
	}
}

static const struct dict_sql_map *
sql_dict_find_map(struct sql_dict *dict, const char *path,
		  ARRAY_TYPE(const_string) *pattern_values)
{
	const struct dict_sql_map *maps;
	unsigned int i, count;
	size_t len;

	t_array_init(pattern_values, dict->set->max_pattern_fields_count);
	maps = array_get(&dict->set->maps, &count);
	for (i = 0; i < count; i++) {
		if (dict_sql_map_match(&maps[i], path, pattern_values,
				       &len, &len, FALSE, FALSE))
			return &maps[i];
	}
	return NULL;
}

static void
sql_dict_statement_bind(struct sql_statement *stmt, unsigned int column_idx,
			const struct sql_dict_param *param)
{
	switch (param->value_type) {
	case DICT_SQL_TYPE_STRING:
		sql_statement_bind_str(stmt, column_idx, param->value_str);
		break;
	case DICT_SQL_TYPE_INT:
	case DICT_SQL_TYPE_UINT:
		sql_statement_bind_int64(stmt, column_idx, param->value_int64);
		break;
	case DICT_SQL_TYPE_DOUBLE:
		sql_statement_bind_double(stmt, column_idx, param->value_double);
		break;
	case DICT_SQL_TYPE_HEXBLOB:
		sql_statement_bind_binary(stmt, column_idx, param->value_binary,
					  param->value_binary_size);
		break;
	case DICT_SQL_TYPE_UUID:
		sql_statement_bind_uuid(stmt, column_idx, param->value_uuid);
		break;
	case DICT_SQL_TYPE_COUNT:
		i_unreached();
	}
}

static struct sql_statement *
sql_dict_statement_init(struct sql_dict *dict, const char *query,
			const ARRAY_TYPE(sql_dict_param) *params)
{
	struct sql_statement *stmt;
	struct sql_prepared_statement *prep_stmt;
	const struct sql_dict_param *param;

	if ((sql_get_flags(dict->db) & SQL_DB_FLAG_PREP_STATEMENTS) != 0) {
		prep_stmt = sql_prepared_statement_init(dict->db, query);
		stmt = sql_statement_init_prepared(prep_stmt);
		sql_prepared_statement_unref(&prep_stmt);
	} else {
		/* Prepared statements not supported by the backend.
		   Just use regular statements to avoid wasting memory. */
		stmt = sql_statement_init(dict->db, query);
	}

	array_foreach(params, param) {
		sql_dict_statement_bind(stmt, array_foreach_idx(params, param),
					param);
	}
	return stmt;
}

static int
sql_dict_value_get(const struct dict_sql_map *map,
		   enum dict_sql_type value_type, const char *field_name,
		   const char *value, const char *value_suffix,
		   ARRAY_TYPE(sql_dict_param) *params, const char **error_r)
{
	struct sql_dict_param *param;
	buffer_t *buf;

	param = array_append_space(params);
	param->value_type = value_type;

	switch (value_type) {
	case DICT_SQL_TYPE_STRING:
		if (value_suffix[0] != '\0')
			value = t_strconcat(value, value_suffix, NULL);
		param->value_str = value;
		return 0;
	case DICT_SQL_TYPE_INT:
		if (value_suffix[0] != '\0' ||
		    str_to_int64(value, &param->value_int64) < 0) {
			*error_r = t_strdup_printf(
				"%s field's value isn't 64bit signed integer: %s%s (in pattern: %s)",
				field_name, value, value_suffix, map->pattern);
			return -1;
		}
		return 0;
	case DICT_SQL_TYPE_UINT:
		if (value_suffix[0] != '\0' || value[0] == '-' ||
		    str_to_int64(value, &param->value_int64) < 0) {
			*error_r = t_strdup_printf(
				"%s field's value isn't 64bit unsigned integer: %s%s (in pattern: %s)",
				field_name, value, value_suffix, map->pattern);
			return -1;
		}
		return 0;
	case DICT_SQL_TYPE_DOUBLE:
		if (value_suffix[0] != '\0' ||
		    str_to_double(value, &param->value_double) < 0) {
			*error_r = t_strdup_printf(
				"%s field's value isn't a double: %s%s (in pattern: %s)",
				field_name, value, value_suffix, map->pattern);
			return -1;
		}
		return 0;
	case DICT_SQL_TYPE_UUID:
		if (value_suffix[0] != '\0' ||
		    guid_128_from_uuid_string(value, param->value_uuid) < 0) {
			*error_r = t_strdup_printf(
				"%s field's value isn't an uuid: %s%s (in pattern: %s)",
				field_name, value, value_suffix, map->pattern);
			return -1;
		}
		return 0;
	case DICT_SQL_TYPE_HEXBLOB:
		break;
	case DICT_SQL_TYPE_COUNT:
		i_unreached();
	}

	buf = t_buffer_create(strlen(value)/2);
	if (hex_to_binary(value, buf) < 0) {
		/* we shouldn't get untrusted input here. it's also a bit
		   annoying to handle this error. */
		*error_r = t_strdup_printf("%s field's value isn't hexblob: %s (in pattern: %s)",
					   field_name, value, map->pattern);
		return -1;
	}
	str_append(buf, value_suffix);
	param->value_binary = buf->data;
	param->value_binary_size = buf->used;
	return 0;
}

static int
sql_dict_field_get_value(const struct dict_sql_map *map,
			 const struct dict_sql_field *field,
			 const char *value, const char *value_suffix,
			 ARRAY_TYPE(sql_dict_param) *params,
			 const char **error_r)
{
	return sql_dict_value_get(map, field->value_type, field->name,
				  value, value_suffix, params, error_r);
}

static int
sql_dict_where_build(const char *username, const struct dict_sql_map *map,
		     const ARRAY_TYPE(const_string) *values_arr,
		     bool add_username, enum sql_recurse_type recurse_type,
		     string_t *query, ARRAY_TYPE(sql_dict_param) *params,
		     const char **error_r)
{
	const struct dict_sql_field *pattern_fields;
	const char *const *pattern_values;
	unsigned int i, count, count2, exact_count;

	pattern_fields = array_get(&map->pattern_fields, &count);
	pattern_values = array_get(values_arr, &count2);
	/* if we came here from iteration code there may be fewer
	   pattern_values */
	i_assert(count2 <= count);

	if (count2 == 0 && !add_username) {
		/* we want everything */
		return 0;
	}

	str_append(query, " WHERE");
	exact_count = count == count2 && recurse_type != SQL_DICT_RECURSE_NONE ?
		count2-1 : count2;
	if (exact_count != array_count(values_arr)) {
		*error_r = t_strdup_printf("Key continues past the matched pattern %s", map->pattern);
		return -1;
	}

	for (i = 0; i < exact_count; i++) {
		if (i > 0)
			str_append(query, " AND");
		str_printfa(query, " %s = ?", pattern_fields[i].name);
		if (sql_dict_field_get_value(map, &pattern_fields[i],
					     pattern_values[i], "",
					     params, error_r) < 0)
			return -1;
	}
	switch (recurse_type) {
	case SQL_DICT_RECURSE_NONE:
		break;
	case SQL_DICT_RECURSE_ONE:
		if (i > 0)
			str_append(query, " AND");
		if (i < count2) {
			str_printfa(query, " %s LIKE ?", pattern_fields[i].name);
			if (sql_dict_field_get_value(map, &pattern_fields[i],
						     pattern_values[i], "/%",
						     params, error_r) < 0)
				return -1;
			str_printfa(query, " AND %s NOT LIKE ?", pattern_fields[i].name);
			if (sql_dict_field_get_value(map, &pattern_fields[i],
						     pattern_values[i], "/%/%",
						     params, error_r) < 0)
				return -1;
		} else {
			str_printfa(query, " %s LIKE '%%' AND "
				    "%s NOT LIKE '%%/%%'",
				    pattern_fields[i].name,
				    pattern_fields[i].name);
		}
		break;
	case SQL_DICT_RECURSE_FULL:
		if (i < count2) {
			if (i > 0)
				str_append(query, " AND");
			str_printfa(query, " %s LIKE ",
				    pattern_fields[i].name);
			if (sql_dict_field_get_value(map, &pattern_fields[i],
						     pattern_values[i], "/%",
						     params, error_r) < 0)
				return -1;
		}
		break;
	}
	if (add_username) {
		struct sql_dict_param *param = array_append_space(params);
		if (count2 > 0)
			str_append(query, " AND");
		str_printfa(query, " %s = ?", map->username_field);
		param->value_type = DICT_SQL_TYPE_STRING;
		param->value_str = t_strdup(username);
	}
	return 0;
}

static int
sql_lookup_get_query(struct sql_dict *dict,
		     const struct dict_op_settings *set,
		     const char *key,
		     const struct dict_sql_map **map_r,
		     struct sql_statement **stmt_r,
		     const char **error_r)
{
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) pattern_values;
	const char *error;

	map = *map_r = sql_dict_find_map(dict, key, &pattern_values);
	if (map == NULL) {
		*error_r = t_strdup_printf(
			"sql dict lookup: Invalid/unmapped key: %s", key);
		return -1;
	}

	string_t *query = t_str_new(256);
	ARRAY_TYPE(sql_dict_param) params;
	t_array_init(&params, 4);
	str_append(query, "SELECT ");
	if (map->expire_field != NULL)
		str_printfa(query, "%s,", map->expire_field);
	str_printfa(query, "%s FROM %s%s",
		    map->value_field, sql_db_table_prefix(dict->db), map->table);
	if (sql_dict_where_build(set->username, map, &pattern_values,
				 key[0] == DICT_PATH_PRIVATE[0],
				 SQL_DICT_RECURSE_NONE, query,
				 &params, &error) < 0) {
		*error_r = t_strdup_printf(
			"sql dict lookup: Failed to lookup key %s: %s", key, error);
		return -1;
	}
	*stmt_r = sql_dict_statement_init(dict, str_c(query), &params);
	return 0;
}

static const char *
sql_dict_result_unescape(enum dict_sql_type type, pool_t pool,
			 struct sql_result *result, unsigned int result_idx)
{
	const unsigned char *data;
	size_t size;
	const char *value;
	guid_128_t guid;
	string_t *str;

	switch (type) {
	case DICT_SQL_TYPE_STRING:
	case DICT_SQL_TYPE_INT:
	case DICT_SQL_TYPE_UINT:
	case DICT_SQL_TYPE_DOUBLE:
		value = sql_result_get_field_value(result, result_idx);
		return value == NULL ? "" : p_strdup(pool, value);
	case DICT_SQL_TYPE_UUID:
		value = sql_result_get_field_value(result, result_idx);
		if (value == NULL)
			return "";
		guid_128_from_uuid_string(value, guid);
		return guid_128_to_uuid_string(guid, FORMAT_RECORD);
	case DICT_SQL_TYPE_HEXBLOB:
		break;
	case DICT_SQL_TYPE_COUNT:
		i_unreached();
	}

	data = sql_result_get_field_value_binary(result, result_idx, &size);
	str = str_new(pool, size*2 + 1);
	binary_to_hex_append(str, data, size);
	return str_c(str);
}

static const char *const *
sql_dict_result_unescape_values(const struct dict_sql_map *map, pool_t pool,
				struct sql_result *result)
{
	const char **values;
	unsigned int i, first_sql_idx = 0;

	values = p_new(pool, const char *, map->values_count + 1);
	if (map->expire_field != NULL) {
		/* don't include expire_field in results */
		first_sql_idx++;
	}
	for (i = 0; i < map->values_count; i++) {
		values[i] = sql_dict_result_unescape(map->value_types[i],
						     pool, result,
						     first_sql_idx + i);
	}
	return values;
}

static const char *
sql_dict_result_unescape_field(const struct dict_sql_map *map, pool_t pool,
			       struct sql_result *result, unsigned int result_idx,
			       unsigned int sql_field_idx)
{
	const struct dict_sql_field *sql_field;

	sql_field = array_idx(&map->pattern_fields, sql_field_idx);
	return sql_dict_result_unescape(sql_field->value_type, pool,
					result, result_idx);
}

static int
sql_dict_result_next_row(const struct dict_sql_map *map,
			 struct sql_result *result)
{
	int ret;

	while ((ret = sql_result_next_row(result)) == SQL_RESULT_NEXT_OK &&
	       map->expire_field != NULL) {
		const char *expire_value =
			sql_result_get_field_value(result, 0);
		time_t expire_timestamp;

		if (expire_value == NULL ||
		    str_to_time(expire_value, &expire_timestamp) < 0 ||
		    expire_timestamp > ioloop_time)
			break;
		/* expired - jump to the next row */
	}
	return ret;
}

static int sql_dict_lookup(struct dict *_dict, const struct dict_op_settings *set,
			   pool_t pool, const char *key,
			   const char *const **values_r, const char **error_r)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	const struct dict_sql_map *map;
	struct sql_statement *stmt;
	struct sql_result *result = NULL;
	int ret;

	if (sql_lookup_get_query(dict, set, key, &map, &stmt, error_r) < 0)
		return -1;

	result = sql_statement_query_s(&stmt);
	ret = sql_dict_result_next_row(map, result);
	if (ret < 0) {
		*error_r = t_strdup_printf("dict sql lookup failed: %s",
					   sql_result_get_error(result));
	} else if (ret > 0) {
		*values_r = sql_dict_result_unescape_values(map, pool, result);
	}

	sql_result_unref(result);
	return ret;
}

struct sql_dict_lookup_context {
	const struct dict_sql_map *map;
	dict_lookup_callback_t *callback;
	void *context;
};

static void
sql_dict_lookup_async_callback(struct sql_result *sql_result,
			       struct sql_dict_lookup_context *ctx)
{
	struct dict_lookup_result result;

	i_zero(&result);
	result.ret = sql_dict_result_next_row(ctx->map, sql_result);
	if (result.ret < 0)
		result.error = sql_result_get_error(sql_result);
	else if (result.ret > 0) {
		result.values = sql_dict_result_unescape_values(ctx->map,
			pool_datastack_create(), sql_result);
		result.value = result.values[0];
		if (result.value == NULL) {
			/* NULL value returned. we'll treat this as
			   "not found", which is probably what is usually
			   wanted. */
			result.ret = 0;
		}
	}
	ctx->callback(&result, ctx->context);

	i_free(ctx);
}

static void
sql_dict_lookup_async(struct dict *_dict,
		      const struct dict_op_settings *set,
		      const char *key,
		      dict_lookup_callback_t *callback, void *context)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	const struct dict_sql_map *map;
	struct sql_dict_lookup_context *ctx;
	struct sql_statement *stmt;
	const char *error;

	if (sql_lookup_get_query(dict, set, key, &map, &stmt, &error) < 0) {
		struct dict_lookup_result result;

		i_zero(&result);
		result.ret = -1;
		result.error = error;
		callback(&result, context);
	} else {
		ctx = i_new(struct sql_dict_lookup_context, 1);
		ctx->callback = callback;
		ctx->context = context;
		ctx->map = map;
		sql_statement_query(&stmt, sql_dict_lookup_async_callback, ctx);
	}
}

static const struct dict_sql_map *
sql_dict_iterate_find_next_map(struct sql_dict_iterate_context *ctx,
			       ARRAY_TYPE(const_string) *pattern_values)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	const struct dict_sql_map *maps;
	unsigned int i, count;
	size_t pat_len, path_len;
	bool recurse = (ctx->flags & DICT_ITERATE_FLAG_RECURSE) != 0;

	t_array_init(pattern_values, dict->set->max_pattern_fields_count);
	maps = array_get(&dict->set->maps, &count);
	for (i = ctx->next_map_idx; i < count; i++) {
		if (dict_sql_map_match(&maps[i], ctx->path,
				       pattern_values, &pat_len, &path_len,
				       TRUE, recurse) &&
		    (recurse ||
		     array_count(pattern_values)+1 >= array_count(&maps[i].pattern_fields))) {
			ctx->key_prefix_len = path_len;
			ctx->pattern_prefix_len = pat_len;
			ctx->next_map_idx = i + 1;

			str_truncate(ctx->key, 0);
			str_append(ctx->key, ctx->path);
			return &maps[i];
		}
	}
	return NULL;
}

static int
sql_dict_iterate_build_next_query(struct sql_dict_iterate_context *ctx,
				  struct sql_statement **stmt_r,
				  const char **error_r)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	const struct dict_op_settings_private *set = &ctx->ctx.set;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) pattern_values;
	const struct dict_sql_field *pattern_fields;
	enum sql_recurse_type recurse_type;
	unsigned int i, count;

	map = sql_dict_iterate_find_next_map(ctx, &pattern_values);
	/* NULL map is allowed if we have already done some lookups */
	if (map == NULL) {
		if (!ctx->allow_null_map) {
			*error_r = "Invalid/unmapped path";
			return -1;
		}
		return 0;
	}

	if (ctx->result != NULL) {
		sql_result_unref(ctx->result);
		ctx->result = NULL;
	}

	string_t *query = t_str_new(256);
	str_append(query, "SELECT ");
	if (map->expire_field != NULL)
		str_printfa(query, "%s,", map->expire_field);
	if ((ctx->flags & DICT_ITERATE_FLAG_NO_VALUE) == 0)
		str_printfa(query, "%s,", map->value_field);

	/* get all missing fields */
	pattern_fields = array_get(&map->pattern_fields, &count);
	i = array_count(&pattern_values);
	if (i == count) {
		/* we always want to know the last field since we're
		   iterating its children */
		i_assert(i > 0);
		i--;
	}
	ctx->sql_fields_start_idx = i;
	for (; i < count; i++)
		str_printfa(query, "%s,", pattern_fields[i].name);
	str_truncate(query, str_len(query)-1);

	str_printfa(query, " FROM %s%s", sql_db_table_prefix(dict->db), map->table);

	if ((ctx->flags & DICT_ITERATE_FLAG_RECURSE) != 0)
		recurse_type = SQL_DICT_RECURSE_FULL;
	else if ((ctx->flags & DICT_ITERATE_FLAG_EXACT_KEY) != 0)
		recurse_type = SQL_DICT_RECURSE_NONE;
	else
		recurse_type = SQL_DICT_RECURSE_ONE;

	ARRAY_TYPE(sql_dict_param) params;
	t_array_init(&params, 4);
	bool add_username = (ctx->path[0] == DICT_PATH_PRIVATE[0]);
	if (sql_dict_where_build(set->username, map, &pattern_values, add_username,
				 recurse_type, query, &params, error_r) < 0)
		return -1;

	if ((ctx->flags & DICT_ITERATE_FLAG_SORT_BY_KEY) != 0) {
		str_append(query, " ORDER BY ");
		for (i = 0; i < count; i++) {
			str_printfa(query, "%s", pattern_fields[i].name);
			if (i < count-1)
				str_append_c(query, ',');
		}
	} else if ((ctx->flags & DICT_ITERATE_FLAG_SORT_BY_VALUE) != 0)
		str_printfa(query, " ORDER BY %s", map->value_field);

	if (ctx->ctx.max_rows > 0) {
		i_assert(ctx->ctx.row_count < ctx->ctx.max_rows);
		str_printfa(query, " LIMIT %"PRIu64,
			    ctx->ctx.max_rows - ctx->ctx.row_count);
	}

	*stmt_r = sql_dict_statement_init(dict, str_c(query), &params);
	ctx->map = map;
	return 1;
}

static void sql_dict_iterate_callback(struct sql_result *result,
				      struct sql_dict_iterate_context *ctx)
{
	if (!ctx->destroyed) {
		sql_result_ref(result);
		ctx->result = result;
		if (ctx->ctx.async_callback != NULL && !ctx->synchronous_result)
			ctx->ctx.async_callback(ctx->ctx.async_context);
	}

	pool_t pool_copy = ctx->pool;
	pool_unref(&pool_copy);
}

static int sql_dict_iterate_next_query(struct sql_dict_iterate_context *ctx)
{
	struct sql_statement *stmt;
	const char *error;
	int ret;

	ret = sql_dict_iterate_build_next_query(ctx, &stmt, &error);
	if (ret <= 0) {
		/* this is expected error */
		if (ret == 0)
			return ret;
		/* failed */
		ctx->error = p_strdup_printf(ctx->pool,
			"sql dict iterate failed for %s: %s",
			ctx->path, error);
		return -1;
	}

	if ((ctx->flags & DICT_ITERATE_FLAG_ASYNC) == 0) {
		ctx->result = sql_statement_query_s(&stmt);
	} else {
		i_assert(ctx->result == NULL);
		ctx->synchronous_result = TRUE;
		pool_ref(ctx->pool);
		sql_statement_query(&stmt, sql_dict_iterate_callback, ctx);
		ctx->synchronous_result = FALSE;
	}
	return ret;
}

static struct dict_iterate_context *
sql_dict_iterate_init(struct dict *_dict,
		      const struct dict_op_settings *set ATTR_UNUSED,
		      const char *path, enum dict_iterate_flags flags)
{
	struct sql_dict_iterate_context *ctx;
	pool_t pool;

	pool = pool_alloconly_create("sql dict iterate", 512);
	ctx = p_new(pool, struct sql_dict_iterate_context, 1);
	ctx->ctx.dict = _dict;
	ctx->pool = pool;
	ctx->flags = flags;

	ctx->path = p_strdup(pool, path);

	ctx->key = str_new(pool, 256);
	ctx->value_pool = pool_alloconly_create("sql dict iterate value", 256);
	return &ctx->ctx;
}

static bool sql_dict_iterate(struct dict_iterate_context *_ctx,
			     const char **key_r, const char *const **values_r)
{
	struct sql_dict_iterate_context *ctx =
		(struct sql_dict_iterate_context *)_ctx;
	const char *p, *value;
	unsigned int i, sql_field_i, count;
	int ret;

	_ctx->has_more = FALSE;
	if (ctx->error != NULL)
		return FALSE;
	if (!ctx->iter_query_sent) {
		ctx->iter_query_sent = TRUE;
		if (sql_dict_iterate_next_query(ctx) <= 0)
			return FALSE;
	}

	if (ctx->result == NULL) {
		/* wait for async lookup to finish */
		i_assert((ctx->flags & DICT_ITERATE_FLAG_ASYNC) != 0);
		_ctx->has_more = TRUE;
		return FALSE;
	}

	ret = sql_dict_result_next_row(ctx->map, ctx->result);
	while (ret == SQL_RESULT_NEXT_MORE) {
		if ((ctx->flags & DICT_ITERATE_FLAG_ASYNC) == 0)
			sql_result_more_s(&ctx->result);
		else {
			/* get more results asynchronously */
			ctx->synchronous_result = TRUE;
			pool_ref(ctx->pool);
			sql_result_more(&ctx->result, sql_dict_iterate_callback, ctx);
			ctx->synchronous_result = FALSE;
			if (ctx->result == NULL) {
				_ctx->has_more = TRUE;
				return FALSE;
			}
		}
		ret = sql_dict_result_next_row(ctx->map, ctx->result);
	}
	if (ret == 0) {
		/* see if there are more results in the next map.
		   don't do it if we're looking for an exact match, since we
		   already should have handled it. */
		if ((ctx->flags & DICT_ITERATE_FLAG_EXACT_KEY) != 0)
			return FALSE;
		ctx->iter_query_sent = FALSE;
		/* we have gotten *SOME* results, so can allow
		   unmapped next key now. */
		ctx->allow_null_map = TRUE;
		return sql_dict_iterate(_ctx, key_r, values_r);
	}
	if (ret < 0) {
		ctx->error = p_strdup_printf(ctx->pool,
			"dict sql iterate failed: %s",
			sql_result_get_error(ctx->result));
		return FALSE;
	}

	/* convert fetched row to dict key */
	str_truncate(ctx->key, ctx->key_prefix_len);
	if (ctx->key_prefix_len > 0 &&
	    str_c(ctx->key)[ctx->key_prefix_len-1] != '/')
		str_append_c(ctx->key, '/');

	count = sql_result_get_fields_count(ctx->result);
	i = (ctx->flags & DICT_ITERATE_FLAG_NO_VALUE) != 0 ? 0 :
		ctx->map->values_count;
	sql_field_i = ctx->sql_fields_start_idx;
	for (p = ctx->map->pattern + ctx->pattern_prefix_len; *p != '\0'; p++) {
		if (*p != '$')
			str_append_c(ctx->key, *p);
		else {
			i_assert(i < count);
			value = sql_dict_result_unescape_field(ctx->map,
					pool_datastack_create(), ctx->result, i, sql_field_i);
			if (value != NULL)
				str_append(ctx->key, value);
			i++; sql_field_i++;
		}
	}

	*key_r = str_c(ctx->key);
	if ((ctx->flags & DICT_ITERATE_FLAG_NO_VALUE) == 0) {
		p_clear(ctx->value_pool);
		*values_r = sql_dict_result_unescape_values(ctx->map,
				ctx->value_pool, ctx->result);
	}
	return TRUE;
}

static int sql_dict_iterate_deinit(struct dict_iterate_context *_ctx,
				   const char **error_r)
{
	struct sql_dict_iterate_context *ctx =
		(struct sql_dict_iterate_context *)_ctx;
	int ret = ctx->error != NULL ? -1 : 0;

	*error_r = t_strdup(ctx->error);
	if (ctx->result != NULL)
		sql_result_unref(ctx->result);
	ctx->destroyed = TRUE;

	pool_unref(&ctx->value_pool);
	pool_t pool_copy = ctx->pool;
	pool_unref(&pool_copy);
	return ret;
}

static struct dict_transaction_context *
sql_dict_transaction_init(struct dict *_dict)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	struct sql_dict_transaction_context *ctx;

	ctx = i_new(struct sql_dict_transaction_context, 1);
	ctx->ctx.dict = _dict;
	ctx->sql_ctx = sql_transaction_begin(dict->db);

	return &ctx->ctx;
}

static void sql_dict_transaction_free(struct sql_dict_transaction_context *ctx)
{
	if (array_is_created(&ctx->prev_inc))
		sql_dict_prev_inc_free(ctx);
	if (array_is_created(&ctx->prev_set))
		sql_dict_prev_set_free(ctx);

	pool_unref(&ctx->inc_row_pool);
	i_free(ctx->error);
	i_free(ctx);
}

static bool
sql_dict_transaction_has_nonexistent(struct sql_dict_transaction_context *ctx)
{
	struct sql_dict_inc_row *inc_row;

	for (inc_row = ctx->inc_row; inc_row != NULL; inc_row = inc_row->prev) {
		i_assert(inc_row->rows != UINT_MAX);
		if (inc_row->rows == 0)
			return TRUE;
	}
	return FALSE;
}

static void
sql_dict_transaction_commit_callback(const struct sql_commit_result *sql_result,
				     struct sql_dict_transaction_context *ctx)
{
	struct dict_commit_result result;

	i_zero(&result);
	if (sql_result->error == NULL)
		result.ret = sql_dict_transaction_has_nonexistent(ctx) ?
			DICT_COMMIT_RET_NOTFOUND : DICT_COMMIT_RET_OK;
	else {
		result.error = t_strdup_printf("sql dict: commit failed: %s",
					       sql_result->error);
		switch (sql_result->error_type) {
		case SQL_RESULT_ERROR_TYPE_UNKNOWN:
		default:
			result.ret = DICT_COMMIT_RET_FAILED;
			break;
		case SQL_RESULT_ERROR_TYPE_WRITE_UNCERTAIN:
			result.ret = DICT_COMMIT_RET_WRITE_UNCERTAIN;
			break;
		}
	}

	if (ctx->async_callback != NULL)
		ctx->async_callback(&result, ctx->async_context);
	else if (result.ret < 0)
		e_error(ctx->ctx.event, "%s", result.error);
	sql_dict_transaction_free(ctx);
}

static void
sql_dict_transaction_commit(struct dict_transaction_context *_ctx, bool async,
			    dict_transaction_commit_callback_t *callback,
			    void *context)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	const char *error;
	struct dict_commit_result result;

	/* flush any pending set/inc */
	if (array_is_created(&ctx->prev_inc))
		sql_dict_prev_inc_flush(ctx);
	if (array_is_created(&ctx->prev_set))
		sql_dict_prev_set_flush(ctx);

	/* note that the above calls might still set ctx->error */
	i_zero(&result);
	result.ret = DICT_COMMIT_RET_FAILED;
	result.error = t_strdup(ctx->error);

	if (ctx->error != NULL) {
		sql_transaction_rollback(&ctx->sql_ctx);
	} else if (!_ctx->changed) {
		/* nothing changed, no need to commit */
		sql_transaction_rollback(&ctx->sql_ctx);
		result.ret = DICT_COMMIT_RET_OK;
	} else if (async) {
		ctx->async_callback = callback;
		ctx->async_context = context;
		sql_transaction_commit(&ctx->sql_ctx,
			sql_dict_transaction_commit_callback, ctx);
		return;
	} else if (sql_transaction_commit_s(&ctx->sql_ctx, &error) < 0) {
		result.error = t_strdup_printf(
			"sql dict: commit failed: %s", error);
	} else {
		if (sql_dict_transaction_has_nonexistent(ctx))
			result.ret = DICT_COMMIT_RET_NOTFOUND;
		else
			result.ret = DICT_COMMIT_RET_OK;
	}
	sql_dict_transaction_free(ctx);

	callback(&result, context);
}

static void sql_dict_transaction_rollback(struct dict_transaction_context *_ctx)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;

	sql_transaction_rollback(&ctx->sql_ctx);
	sql_dict_transaction_free(ctx);
}

static struct sql_statement *
sql_dict_transaction_stmt_init(struct sql_dict_transaction_context *ctx,
			       const char *query,
			       const ARRAY_TYPE(sql_dict_param) *params)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	struct sql_statement *stmt =
		sql_dict_statement_init(dict, query, params);

	if (ctx->ctx.timestamp.tv_sec != 0)
		sql_statement_set_timestamp(stmt, &ctx->ctx.timestamp);
	if (ctx->ctx.set.hide_log_values)
		sql_statement_set_no_log_expanded_values(stmt, ctx->ctx.set.hide_log_values);
	return stmt;
}

struct dict_sql_build_query_field {
	const struct dict_sql_map *map;
	const char *value;
};

struct dict_sql_build_query {
	struct sql_dict *dict;

	ARRAY(struct dict_sql_build_query_field) fields;
	const ARRAY_TYPE(const_string) *pattern_values;
	bool add_username;
};

static int sql_dict_set_query(struct sql_dict_transaction_context *ctx,
			      const struct dict_sql_build_query *build,
			      struct sql_statement **stmt_r,
			      const char **error_r)
{
	struct sql_dict *dict = build->dict;
	const struct dict_sql_build_query_field *fields;
	const struct dict_sql_field *pattern_fields;
	ARRAY_TYPE(sql_dict_param) params;
	const char *const *pattern_values;
	unsigned int i, field_count, count, count2;
	string_t *prefix, *suffix;
	time_t expire_timestamp = 0;

	fields = array_get(&build->fields, &field_count);
	i_assert(field_count > 0);

	if (fields[0].map->expire_field != NULL &&
	    ctx->ctx.set.expire_secs > 0)
		expire_timestamp = ioloop_time + ctx->ctx.set.expire_secs;

	t_array_init(&params, 4);
	prefix = t_str_new(64);
	suffix = t_str_new(256);
	/* SQL table is guaranteed to be the same for all fields.
	   Build all the SQL field names into prefix and '?' placeholders for
	   each value into the suffix. The actual field values will be added
	   into params[]. */
	str_printfa(prefix, "INSERT INTO %s%s",
		    sql_db_table_prefix(dict->db), fields[0].map->table);
	str_append(prefix, " (");
	str_append(suffix, ") VALUES (");
	for (i = 0; i < field_count; i++) {
		if (i > 0) {
			str_append_c(prefix, ',');
			str_append_c(suffix, ',');
		}
		str_append(prefix, t_strcut(fields[i].map->value_field, ','));

		enum dict_sql_type value_type =
			fields[i].map->value_types[0];
		str_append_c(suffix, '?');
		if (sql_dict_value_get(fields[i].map,
				       value_type, "value", fields[i].value,
				       "", &params, error_r) < 0)
			return -1;
	}
	if (build->add_username) {
		struct sql_dict_param *param = array_append_space(&params);
		str_printfa(prefix, ",%s", fields[0].map->username_field);
		str_append(suffix, ",?");
		param->value_type = DICT_SQL_TYPE_STRING;
		param->value_str = ctx->ctx.set.username;
	}
	if (expire_timestamp != 0) {
		struct sql_dict_param *param = array_append_space(&params);
		str_printfa(prefix, ",%s", fields[0].map->expire_field);
		str_append(suffix, ",?");
		param->value_type = DICT_SQL_TYPE_UINT;
		param->value_int64 = expire_timestamp;
	}

	/* add the variable fields that were parsed from the path */
	pattern_fields = array_get(&fields[0].map->pattern_fields, &count);
	pattern_values = array_get(build->pattern_values, &count2);
	i_assert(count == count2);
	for (i = 0; i < count; i++) {
		str_printfa(prefix, ",%s", pattern_fields[i].name);
		str_append(suffix, ",?");
		if (sql_dict_field_get_value(fields[0].map, &pattern_fields[i],
					     pattern_values[i], "",
					     &params, error_r) < 0)
			return -1;
	}

	str_append_str(prefix, suffix);
	str_append_c(prefix, ')');

	enum sql_db_flags flags = sql_get_flags(dict->db);
	if ((flags & SQL_DB_FLAG_ON_DUPLICATE_KEY) != 0)
		str_append(prefix, " ON DUPLICATE KEY UPDATE ");
	else if ((flags & SQL_DB_FLAG_ON_CONFLICT_DO) != 0) {
		str_append(prefix, " ON CONFLICT (");
		for (i = 0; i < count; i++) {
			if (i > 0)
				str_append_c(prefix, ',');
			str_append(prefix, pattern_fields[i].name);
		}
		if (build->add_username) {
			if (count > 0)
				str_append_c(prefix, ',');
			str_append(prefix, fields[0].map->username_field);
		}
		str_append(prefix, ") DO UPDATE SET ");
	} else {
		*stmt_r = sql_dict_transaction_stmt_init(ctx, str_c(prefix), &params);
		return 0;
	}

	/* If the row already exists, UPDATE it instead. The pattern_values
	   don't need to be updated here, because they are expected to be part
	   of the row's primary key. */
	for (i = 0; i < field_count; i++) {
		const char *first_value_field =
			t_strcut(fields[i].map->value_field, ',');
		if (i > 0)
			str_append_c(prefix, ',');
		str_append(prefix, first_value_field);
		str_append_c(prefix, '=');

		enum dict_sql_type value_type =
			fields[i].map->value_types[0];
		str_append_c(prefix, '?');
		if (sql_dict_value_get(fields[i].map,
				       value_type, "value", fields[i].value,
				       "", &params, error_r) < 0)
			return -1;
	}
	if (expire_timestamp != 0) {
		str_printfa(prefix, ",%s=?", fields[0].map->expire_field);
		struct sql_dict_param *param = array_append_space(&params);
		param->value_type = DICT_SQL_TYPE_UINT;
		param->value_int64 = expire_timestamp;
	}
	*stmt_r = sql_dict_transaction_stmt_init(ctx, str_c(prefix), &params);
	return 0;
}

static int
sql_dict_update_query(const struct dict_sql_build_query *build,
		      const struct dict_op_settings_private *set,
		      const char **query_r, ARRAY_TYPE(sql_dict_param) *params,
		      const char **error_r)
{
	const struct dict_sql_build_query_field *fields;
	unsigned int i, field_count;
	string_t *query;

	fields = array_get(&build->fields, &field_count);
	i_assert(field_count > 0);

	query = t_str_new(64);
	str_printfa(query, "UPDATE %s%s SET ",
		    sql_db_table_prefix(build->dict->db), fields[0].map->table);
	for (i = 0; i < field_count; i++) {
		const char *first_value_field =
			t_strcut(fields[i].map->value_field, ',');
		if (i > 0)
			str_append_c(query, ',');
		str_printfa(query, "%s=%s+?", first_value_field,
			    first_value_field);
	}

	if (sql_dict_where_build(set->username, fields[0].map, build->pattern_values,
				 build->add_username, SQL_DICT_RECURSE_NONE,
				 query, params, error_r) < 0)
		return -1;
	*query_r = str_c(query);
	return 0;
}

static void sql_dict_prev_set_free(struct sql_dict_transaction_context *ctx)
{
	struct sql_dict_prev *prev_set;

	array_foreach_modifiable(&ctx->prev_set, prev_set) {
		i_free(prev_set->value.str);
		i_free(prev_set->key);
	}
	array_free(&ctx->prev_set);
}

static void sql_dict_prev_set_flush(struct sql_dict_transaction_context *ctx)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	const struct sql_dict_prev *prev_sets;
	unsigned int count;
	struct sql_statement *stmt;
	ARRAY_TYPE(const_string) pattern_values;
	struct dict_sql_build_query build;
	struct dict_sql_build_query_field *field;
	const char *error;

	i_assert(array_is_created(&ctx->prev_set));

	if (ctx->error != NULL) {
		sql_dict_prev_set_free(ctx);
		return;
	}

	prev_sets = array_get(&ctx->prev_set, &count);
	i_assert(count > 0);

	/* Get the variable values from the dict path. We already verified that
	   these are all exactly the same for everything in prev_sets. */
	if (sql_dict_find_map(dict, prev_sets[0].key, &pattern_values) == NULL)
		i_unreached(); /* this was already checked */

	i_zero(&build);
	build.dict = dict;
	build.pattern_values = &pattern_values;
	build.add_username = (prev_sets[0].key[0] == DICT_PATH_PRIVATE[0]);

	/* build.fields[] is used to get the map { value_field } for the
	   SQL field names, as well as the values for them.

	   Example: INSERT INTO ... (build.fields[0].map->value_field,
	   ...[1], ...) VALUES (build.fields[0].value, ...[1], ...) */
	t_array_init(&build.fields, count);
	for (unsigned int i = 0; i < count; i++) {
		i_assert(build.add_username ==
			 (prev_sets[i].key[0] == DICT_PATH_PRIVATE[0]));
		field = array_append_space(&build.fields);
		field->map = prev_sets[i].map;
		field->value = prev_sets[i].value.str;
	}

	if (sql_dict_set_query(ctx, &build, &stmt, &error) < 0) {
		ctx->error = i_strdup_printf(
			"dict-sql: Failed to set %u fields (first %s): %s",
			count, prev_sets[0].key, error);
	} else {
		sql_update_stmt(ctx->sql_ctx, &stmt);
	}
	sql_dict_prev_set_free(ctx);
}

static void sql_dict_unset(struct dict_transaction_context *_ctx,
			   const char *key)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	struct sql_dict *dict = (struct sql_dict *)_ctx->dict;
	const struct dict_op_settings_private *set = &_ctx->set;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) pattern_values;
	string_t *query = t_str_new(256);
	ARRAY_TYPE(sql_dict_param) params;
	const char *error;

	if (ctx->error != NULL)
		return;

	/* In theory we could unset one of the previous set/incs in this
	   same transaction, so flush them first. */
	if (array_is_created(&ctx->prev_inc))
		sql_dict_prev_inc_flush(ctx);
	if (array_is_created(&ctx->prev_set))
		sql_dict_prev_set_flush(ctx);

	map = sql_dict_find_map(dict, key, &pattern_values);
	if (map == NULL) {
		ctx->error = i_strdup_printf("dict-sql: Invalid/unmapped key: %s", key);
		return;
	}

	str_printfa(query, "DELETE FROM %s%s", sql_db_table_prefix(dict->db), map->table);
	t_array_init(&params, 4);
	if (sql_dict_where_build(set->username, map, &pattern_values,
				 key[0] == DICT_PATH_PRIVATE[0],
				 SQL_DICT_RECURSE_NONE, query,
				 &params, &error) < 0) {
		ctx->error = i_strdup_printf(
			"dict-sql: Failed to delete %s: %s", key, error);
	} else {
		struct sql_statement *stmt =
			sql_dict_transaction_stmt_init(ctx, str_c(query), &params);
		sql_update_stmt(ctx->sql_ctx, &stmt);
	}
}

static unsigned int *
sql_dict_next_inc_row(struct sql_dict_transaction_context *ctx)
{
	struct sql_dict_inc_row *row;

	if (ctx->inc_row_pool == NULL) {
		ctx->inc_row_pool =
			pool_alloconly_create("sql dict inc rows", 128);
	}
	row = p_new(ctx->inc_row_pool, struct sql_dict_inc_row, 1);
	row->prev = ctx->inc_row;
	row->rows = UINT_MAX;
	ctx->inc_row = row;
	return &row->rows;
}

static void sql_dict_prev_inc_free(struct sql_dict_transaction_context *ctx)
{
	struct sql_dict_prev *prev_inc;

	array_foreach_modifiable(&ctx->prev_inc, prev_inc)
		i_free(prev_inc->key);
	array_free(&ctx->prev_inc);
}

static void sql_dict_prev_inc_flush(struct sql_dict_transaction_context *ctx)
{
	struct sql_dict *dict = (struct sql_dict *)ctx->ctx.dict;
	const struct dict_op_settings_private *set = &ctx->ctx.set;
	const struct sql_dict_prev *prev_incs;
	unsigned int count;
	ARRAY_TYPE(const_string) pattern_values;
	struct dict_sql_build_query build;
	struct dict_sql_build_query_field *field;
	ARRAY_TYPE(sql_dict_param) params;
	struct sql_dict_param *param;
	const char *query, *error;

	i_assert(array_is_created(&ctx->prev_inc));

	if (ctx->error != NULL) {
		sql_dict_prev_inc_free(ctx);
		return;
	}

	prev_incs = array_get(&ctx->prev_inc, &count);
	i_assert(count > 0);

	/* Get the variable values from the dict path. We already verified that
	   these are all exactly the same for everything in prev_incs. */
	if (sql_dict_find_map(dict, prev_incs[0].key, &pattern_values) == NULL)
		i_unreached(); /* this was already checked */

	i_zero(&build);
	build.dict = dict;
	build.pattern_values = &pattern_values;
	build.add_username = (prev_incs[0].key[0] == DICT_PATH_PRIVATE[0]);

	/* build.fields[] is an array of maps, which are used to get the
	   map { value_field } for the SQL field names.

	   params[] specifies the list of values to use for each field.

	   Example: UPDATE .. SET build.fields[0].map->value_field =
	   ...->value_field + params[0]->value_int64, ...[1]... */
	t_array_init(&build.fields, count);
	t_array_init(&params, count);
	for (unsigned int i = 0; i < count; i++) {
		i_assert(build.add_username ==
			 (prev_incs[i].key[0] == DICT_PATH_PRIVATE[0]));
		field = array_append_space(&build.fields);
		field->map = prev_incs[i].map;
		field->value = NULL; /* unused */

		param = array_append_space(&params);
		param->value_type = DICT_SQL_TYPE_INT;
		param->value_int64 = prev_incs[i].value.diff;
	}

	if (sql_dict_update_query(&build, set, &query, &params, &error) < 0) {
		ctx->error = i_strdup_printf(
			"dict-sql: Failed to increase %u fields (first %s): %s",
			count, prev_incs[0].key, error);
	} else {
		struct sql_statement *stmt =
			sql_dict_transaction_stmt_init(ctx, query, &params);
		sql_update_stmt_get_rows(ctx->sql_ctx, &stmt,
					 sql_dict_next_inc_row(ctx));
	}
	sql_dict_prev_inc_free(ctx);
}

static bool
sql_dict_maps_are_mergeable(struct sql_dict *dict,
			    const struct sql_dict_prev *prev1,
			    const struct dict_sql_map *map2,
			    const char *map2_key,
			    const ARRAY_TYPE(const_string) *map2_pattern_values)
{
	const struct dict_sql_map *map3;
	ARRAY_TYPE(const_string) map1_pattern_values;

	/* sql table names must equal */
	if (strcmp(prev1->map->table, map2->table) != 0)
		return FALSE;
	/* private vs shared prefix must equal */
	if (prev1->key[0] != map2_key[0])
		return FALSE;
	if (prev1->key[0] == DICT_PATH_PRIVATE[0]) {
		/* for private keys, username must equal */
		if (strcmp(prev1->map->username_field, map2->username_field) != 0)
			return FALSE;
	}

	/* variable values in the paths must equal exactly */
	map3 = sql_dict_find_map(dict, prev1->key, &map1_pattern_values);
	i_assert(map3 == prev1->map);

	return array_equal_fn(&map1_pattern_values, map2_pattern_values,
			      i_strcmp_p);
}

static void sql_dict_set(struct dict_transaction_context *_ctx,
			 const char *key, const char *value)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	struct sql_dict *dict = (struct sql_dict *)_ctx->dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) pattern_values;

	if (ctx->error != NULL)
		return;

	/* In theory we could set the previous inc in this same transaction,
	   so flush it first. */
	if (array_is_created(&ctx->prev_inc))
		sql_dict_prev_inc_flush(ctx);

	map = sql_dict_find_map(dict, key, &pattern_values);
	if (map == NULL) {
		ctx->error = i_strdup_printf(
			"sql dict set: Invalid/unmapped key: %s", key);
		return;
	}

	if (array_is_created(&ctx->prev_set) &&
	    !sql_dict_maps_are_mergeable(dict, array_front(&ctx->prev_set),
					 map, key, &pattern_values)) {
		/* couldn't merge to the previous set - flush it */
		sql_dict_prev_set_flush(ctx);
	}

	if (!array_is_created(&ctx->prev_set))
		i_array_init(&ctx->prev_set, 4);
	/* Either this is the first set, or this can be merged with the
	   previous set. */
	struct sql_dict_prev *prev_set = array_append_space(&ctx->prev_set);
	prev_set->map = map;
	prev_set->key = i_strdup(key);
	prev_set->value.str = i_strdup(value);
}

static void sql_dict_atomic_inc(struct dict_transaction_context *_ctx,
				const char *key, long long diff)
{
	struct sql_dict_transaction_context *ctx =
		(struct sql_dict_transaction_context *)_ctx;
	struct sql_dict *dict = (struct sql_dict *)_ctx->dict;
	const struct dict_sql_map *map;
	ARRAY_TYPE(const_string) pattern_values;

	if (ctx->error != NULL)
		return;

	/* In theory we could inc the previous set in this same transaction,
	   so flush it first. */
	if (array_is_created(&ctx->prev_set))
		sql_dict_prev_set_flush(ctx);

	map = sql_dict_find_map(dict, key, &pattern_values);
	if (map == NULL) {
		ctx->error = i_strdup_printf(
			"sql dict atomic inc: Invalid/unmapped key: %s", key);
		return;
	}

	if (array_is_created(&ctx->prev_inc) &&
	    !sql_dict_maps_are_mergeable(dict, array_front(&ctx->prev_inc),
					 map, key, &pattern_values)) {
		/* couldn't merge to the previous inc - flush it */
		sql_dict_prev_inc_flush(ctx);
	}

	if (!array_is_created(&ctx->prev_inc))
		i_array_init(&ctx->prev_inc, 4);
	/* Either this is the first inc, or this can be merged with the
	   previous inc. */
	struct sql_dict_prev *prev_inc = array_append_space(&ctx->prev_inc);
	prev_inc->map = map;
	prev_inc->key = i_strdup(key);
	prev_inc->value.diff = diff;
}

static int
sql_dict_expire_map(struct sql_dict *dict, const struct dict_sql_map *map,
		    const char **error_r)
{
	ARRAY_TYPE(sql_dict_param) params;
	const char *error;

	t_array_init(&params, 1);
	struct sql_dict_param *param = array_append_space(&params);
	param->value_type = DICT_SQL_TYPE_INT;
	param->value_int64 = ioloop_timeval.tv_sec * 1000000 +
		ioloop_timeval.tv_usec;

	struct sql_transaction_context *trans =
		sql_transaction_begin(dict->db);
	const char *query = t_strdup_printf(
		"DELETE FROM %s%s WHERE %s <= ?",
		sql_db_table_prefix(dict->db), map->table, map->expire_field);
	struct sql_statement *stmt =
		sql_dict_statement_init(dict, query, &params);
	sql_update_stmt(trans, &stmt);
	if (sql_transaction_commit_s(&trans, &error) < 0) {
		*error_r = t_strdup_printf(
			"sql dict: commit failed: %s", error);
		return -1;
	}
	return 0;
}

static int sql_dict_expire_scan(struct dict *_dict, const char **error_r)
{
	struct sql_dict *dict = (struct sql_dict *)_dict;
	const struct dict_sql_map *map;
	bool found = FALSE;

	array_foreach(&dict->set->maps, map) {
		if (map->expire_field != NULL) {
			if (sql_dict_expire_map(dict, map, error_r) < 0)
				return -1;
			found = TRUE;
		}
	}
	return found ? 1 : 0;
}

static struct dict sql_dict = {
	.name = "sql",
	.flags = DICT_DRIVER_FLAG_SUPPORT_EXPIRE_SECS,
	.v = {
		.init = sql_dict_init,
		.deinit = sql_dict_deinit,
		.wait = sql_dict_wait,
		.expire_scan = sql_dict_expire_scan,
		.lookup = sql_dict_lookup,
		.iterate_init = sql_dict_iterate_init,
		.iterate = sql_dict_iterate,
		.iterate_deinit = sql_dict_iterate_deinit,
		.transaction_init = sql_dict_transaction_init,
		.transaction_commit = sql_dict_transaction_commit,
		.transaction_rollback = sql_dict_transaction_rollback,
		.set = sql_dict_set,
		.unset = sql_dict_unset,
		.atomic_inc = sql_dict_atomic_inc,
		.lookup_async = sql_dict_lookup_async,
	}
};

static struct dict *dict_sql_drivers;

void dict_sql_register(void)
{
        const struct sql_db *const *drivers;
	unsigned int i, count;

	dict_sql_db_cache = sql_db_cache_init(DICT_SQL_MAX_UNUSED_CONNECTIONS);

	/* @UNSAFE */
	drivers = array_get(&sql_drivers, &count);
	dict_sql_drivers = i_new(struct dict, count + 1);

	for (i = 0; i < count; i++) {
		dict_sql_drivers[i] = sql_dict;
		dict_sql_drivers[i].name = drivers[i]->name;

		dict_driver_register(&dict_sql_drivers[i]);
	}
}

void dict_sql_unregister(void)
{
	int i;

	for (i = 0; dict_sql_drivers[i].name != NULL; i++)
		dict_driver_unregister(&dict_sql_drivers[i]);
	i_free(dict_sql_drivers);
	sql_db_cache_deinit(&dict_sql_db_cache);
	dict_sql_settings_deinit();
}

/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 2013 Digium, Inc.
 *
 * Richard Mudgett <rmudgett@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*!
 * \file
 * \brief Call center agent pool.
 *
 * \author Richard Mudgett <rmudgett@digium.com>
 *
 * See Also:
 * \arg \ref AstCREDITS
 * \arg \ref Config_agent
 */
/*** MODULEINFO
	<depend>res_monitor</depend>
	<support_level>core</support_level>
 ***/


#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision$")

#include "asterisk/module.h"
#include "asterisk/channel.h"
#include "asterisk/config_options.h"
#include "asterisk/astobj2.h"
#include "asterisk/stringfields.h"

/*** DOCUMENTATION
	<application name="AgentLogin" language="en_US">
		<synopsis>
			Call agent login.
		</synopsis>
		<syntax>
			<parameter name="AgentId">
				<para>
	 				If not present the agent is prompted for an identifier.
	 			</para>
			</parameter>
			<parameter name="options">
				<optionlist>
					<option name="s">
						<para>silent login - do not announce the login ok segment after
						agent logged on/off</para>
					</option>
				</optionlist>
			</parameter>
		</syntax>
		<description>
			<para>Login an agent to the system.  Always returns <literal>-1</literal>.
			While logged in, the agent can receive calls and will hear a <literal>beep</literal>
			when a new call comes in.  The agent can dump the call by pressing the star key.</para>
		</description>
		<see-also>
			<ref type="application">Queue</ref>
			<ref type="application">AddQueueMember</ref>
			<ref type="application">RemoveQueueMember</ref>
			<ref type="application">PauseQueueMember</ref>
			<ref type="application">UnpauseQueueMember</ref>
			<ref type="function">AGENT</ref>
			<ref type="filename">agents.conf</ref>
			<ref type="filename">queues.conf</ref>
		</see-also>
	</application>
	<function name="AGENT" language="en_US">
		<synopsis>
			Gets information about an Agent
		</synopsis>
		<syntax argsep=":">
			<parameter name="agentid" required="true" />
			<parameter name="item">
				<para>The valid items to retrieve are:</para>
				<enumlist>
					<enum name="status">
						<para>(default) The status of the agent (LOGGEDIN | LOGGEDOUT)</para>
					</enum>
					<enum name="password">
						<para>The password of the agent</para>
					</enum>
					<enum name="name">
						<para>The name of the agent</para>
					</enum>
					<enum name="mohclass">
						<para>MusicOnHold class</para>
					</enum>
					<enum name="channel">
						<para>The name of the active channel for the Agent (AgentLogin)</para>
					</enum>
					<enum name="fullchannel">
						<para>The untruncated name of the active channel for the Agent (AgentLogin)</para>
					</enum>
				</enumlist>
			</parameter>
		</syntax>
		<description></description>
	</function>
	<manager name="Agents" language="en_US">
		<synopsis>
			Lists agents and their status.
		</synopsis>
		<syntax>
			<xi:include xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])" />
		</syntax>
		<description>
			<para>Will list info about all possible agents.</para>
		</description>
	</manager>
	<manager name="AgentLogoff" language="en_US">
		<synopsis>
			Sets an agent as no longer logged in.
		</synopsis>
		<syntax>
			<xi:include xpointer="xpointer(/docs/manager[@name='Login']/syntax/parameter[@name='ActionID'])" />
			<parameter name="Agent" required="true">
				<para>Agent ID of the agent to log off.</para>
			</parameter>
			<parameter name="Soft">
				<para>Set to <literal>true</literal> to not hangup existing calls.</para>
			</parameter>
		</syntax>
		<description>
			<para>Sets an agent as no longer logged in.</para>
		</description>
	</manager>
 ***/

/* ------------------------------------------------------------------- */

/*! Agent config parameters. */
struct agent_cfg {
	AST_DECLARE_STRING_FIELDS(
		/*! Identification of the agent.  (agents config container key) */
		AST_STRING_FIELD(username);
		/*! Password the agent needs when logging in. */
		AST_STRING_FIELD(password);
		/*! Name of agent for logging and querying purposes */
		AST_STRING_FIELD(full_name);

		/*!
		 * \brief DTMF string for an agent to accept a call.
		 *
		 * \note The channel variable AGENTACCEPTDTMF overrides on login.
		 */
		AST_STRING_FIELD(dtmf_accept);
		/*!
		 * \brief DTMF string for an agent to end a call.
		 *
		 * \note The channel variable AGENTENDDTMF overrides on login.
		 */
		AST_STRING_FIELD(dtmf_end);
		/*! Beep sound file to use.  Alert the agent a call is waiting. */
		AST_STRING_FIELD(beep_sound);
		/*! MOH class to use while agent waiting for call. */
		AST_STRING_FIELD(moh);
		/*! Absolute recording filename directory. (Made to start and end with '/') */
		AST_STRING_FIELD(save_calls_in);
		/*! Recording format filename extension. */
		AST_STRING_FIELD(record_format);
	);
	/*! Agent groups an agent belongs to. */
	ast_group_t group;
	/*!
	 * \brief Number of failed login attempts allowed.
	 *
	 * \note The channel variable AGENTLMAXLOGINTRIES overrides on login.
	 * \note If zero then unlimited attempts.
	 */
	unsigned int max_login_tries;
	/*!
	 * \brief Number of seconds for agent to ack a call before being logged off.
	 *
	 * \note The channel variable AGENTAUTOLOGOFF overrides on login.
	 * \note If zero then timer is disabled.
	 */
	unsigned int auto_logoff;
	/*!
	 * \brief Time after a call in ms before the agent can get a new call.
	 *
	 * \note The channel variable AGENTWRAPUPTIME overrides on login.
	 */
	unsigned int wrapup_time;
	/*!
	 * \brief TRUE if agent needs to ack a call to accept it.
	 *
	 * \note The channel variable AGENTACKCALL overrides on login.
	 */
	int ack_call;
	/*!
	 * \brief TRUE if agent can use DTMF to end a call.
	 *
	 * \note The channel variable AGENTENDCALL overrides on login.
	 */
	int end_call;
	/*! TRUE if agent calls are recorded. */
	int record_agent_calls;
};

/*!
 * \internal
 * \brief Agent config ao2 container sort function.
 * \since 12.0.0
 *
 * \param obj_left pointer to the (user-defined part) of an object.
 * \param obj_right pointer to the (user-defined part) of an object.
 * \param flags flags from ao2_callback()
 *   OBJ_POINTER - if set, 'obj_right', is an object.
 *   OBJ_KEY - if set, 'obj_right', is a search key item that is not an object.
 *   OBJ_PARTIAL_KEY - if set, 'obj_right', is a partial search key item that is not an object.
 *
 * \retval <0 if obj_left < obj_right
 * \retval =0 if obj_left == obj_right
 * \retval >0 if obj_left > obj_right
 */
static int agent_cfg_sort_cmp(const void *obj_left, const void *obj_right, int flags)
{
	const struct agent_cfg *cfg_left = obj_left;
	const struct agent_cfg *cfg_right = obj_right;
	const char *right_key = obj_right;
	int cmp;

	switch (flags & (OBJ_POINTER | OBJ_KEY | OBJ_PARTIAL_KEY)) {
	default:
	case OBJ_POINTER:
		right_key = cfg_right->username;
		/* Fall through */
	case OBJ_KEY:
		cmp = strcmp(cfg_left->username, right_key);
		break;
	case OBJ_PARTIAL_KEY:
		cmp = strncmp(cfg_left->username, right_key, strlen(right_key));
		break;
	}
	return cmp;
}

static void agent_cfg_destructor(void *vdoomed)
{
	struct agent_cfg *doomed = vdoomed;

	ast_string_field_free_memory(doomed);
}

static void *agent_cfg_alloc(const char *name)
{
	struct agent_cfg *cfg;

	cfg = ao2_alloc_options(sizeof(*cfg), agent_cfg_destructor,
		AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!cfg || ast_string_field_init(cfg, 64)) {
		return NULL;
	}
	ast_string_field_set(cfg, username, name);
	return cfg;
}

static void *agent_cfg_find(struct ao2_container *agents, const char *username)
{
	return ao2_find(agents, username, OBJ_KEY);
}

/*! Agents configuration */
struct agents_cfg {
	/*! Master configured agents container. */
	struct ao2_container *agents;
};

static struct aco_type agent_type = {
	.type = ACO_ITEM,
	.name = "agent-id",
	.category_match = ACO_BLACKLIST,
	.category = "^(general|agents)$",
	.item_alloc = agent_cfg_alloc,
	.item_find = agent_cfg_find,
	.item_offset = offsetof(struct agents_cfg, agents),
};

static struct aco_type *agent_types[] = ACO_TYPES(&agent_type);

/* The general category is reserved, but unused */
static struct aco_type general_type = {
	.type = ACO_GLOBAL,
	.name = "global",
	.category_match = ACO_WHITELIST,
	.category = "^general$",
};

static struct aco_file agents_conf = {
	.filename = "agents.conf",
	.types = ACO_TYPES(&general_type, &agent_type),
};

/*
 * BUGBUG must fix config framework loading of multiple files.
 *
 * A reload with multiple files must reload all files if any
 * file has been touched.
 */
/*
 * BUGBUG chan_agent stupidly deals with users.conf.
 *
 * Agents built by users.conf will use defaults except for the
 * three parameters obtained from users.conf.  Also any agent
 * declared by users.conf must not already be declared by
 * agents.conf.
 *
 * [general]
 * hasagent = yes/no (global [user] hasagent=yes value)
 *
 * [user] <- agent-id/username
 * hasagent = yes/no
 * fullname=name
 * secret=password
 *
 *static struct aco_file users_conf = {
 *	.filename = "users.conf",
 *	.preload = { "general", NULL }
 *	.types = ACO_TYPES(&users_type, &users_general_type),
 *};
 *
 *  .files = ACO_FILES(&agents_conf, &users_conf),
 *
 * Will need a preapply config function to create valid users.conf
 * agents in the master agents config container.
 * See verify_default_profiles();
 */

static AO2_GLOBAL_OBJ_STATIC(cfg_handle);

static void agents_cfg_destructor(void *vdoomed)
{
	struct agents_cfg *doomed = vdoomed;

	ao2_cleanup(doomed->agents);
	doomed->agents = NULL;
}

/*!
 * \internal
 * \brief Create struct agents_cfg object.
 * \since 12.0.0
 *
 * \note A lock is not needed for the object or any secondary
 * created cfg objects.  These objects are immutable after the
 * config is loaded and applied.
 *
 * \retval New struct agents_cfg object.
 * \retval NULL on error.
 */
static void *agents_cfg_alloc(void)
{
	struct agents_cfg *cfg;

	cfg = ao2_alloc_options(sizeof(*cfg), agents_cfg_destructor,
		AO2_ALLOC_OPT_LOCK_NOLOCK);
	if (!cfg) {
		return NULL;
	}
	cfg->agents = ao2_container_alloc_rbtree(AO2_ALLOC_OPT_LOCK_NOLOCK,
		AO2_CONTAINER_ALLOC_OPT_DUPS_REJECT, agent_cfg_sort_cmp, NULL);
	return cfg;
}

static void agents_post_apply_config(void);

CONFIG_INFO_STANDARD(cfg_info, cfg_handle, agents_cfg_alloc,
	.files = ACO_FILES(&agents_conf),
	.post_apply_config = agents_post_apply_config,
);

/*!
 * \internal
 * \brief Handle the agent group option.
 * \since 12.0.0
 *
 * \param opt The option being configured
 * \param var The config variable to use to configure \a obj
 * \param obj The object to be configured
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int agent_group_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct agent_cfg *cfg = obj;

/* BUGBUG config framework needs to handle group and groupname parsing. */
	cfg->group = ast_get_group(var->value);
	return 0;
}

/*!
 * \internal
 * \brief Handle the agent savecallsin option.
 * \since 12.0.0
 *
 * \param opt The option being configured
 * \param var The config variable to use to configure \a obj
 * \param obj The object to be configured
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int agent_savecallsin_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct agent_cfg *cfg = obj;
	size_t len;
	int need_leading;
	int need_trailing;

	if (ast_strlen_zero(var->value)) {
		ast_string_field_set(cfg, save_calls_in, "");
		return 0;
	}

	/* Add a leading and/or trailing '/' if needed. */
	len = strlen(var->value);
	need_leading = var->value[0] != '/';
	need_trailing = var->value[len - 1] != '/';
	ast_string_field_build(cfg, save_calls_in, "%s%s%s",
		need_leading ? "/" : "", var->value, need_trailing ? "/" : "");
	return 0;
}

/*!
 * \internal
 * \brief Handle the agent custom_beep option.
 * \since 12.0.0
 *
 * \param opt The option being configured
 * \param var The config variable to use to configure \a obj
 * \param obj The object to be configured
 *
 * \retval 0 on success.
 * \retval -1 on error.
 */
static int agent_custom_beep_handler(const struct aco_option *opt, struct ast_variable *var, void *obj)
{
	struct agent_cfg *cfg = obj;

	if (ast_strlen_zero(var->value)) {
		return -1;
	}

	ast_string_field_set(cfg, beep_sound, "");
	return 0;
}

static void destroy_config(void)
{
	ao2_global_obj_release(cfg_handle);
	aco_info_destroy(&cfg_info);
}

static int load_config(void)
{
	if (aco_info_init(&cfg_info)) {
		return -1;
	}

	/* Agent options */
	aco_option_register(&cfg_info, "maxlogintries", ACO_EXACT, agent_types, "3", OPT_UINT_T, 0, FLDSET(struct agent_cfg, max_login_tries));
	aco_option_register(&cfg_info, "autologoff", ACO_EXACT, agent_types, "0", OPT_UINT_T, 0, FLDSET(struct agent_cfg, auto_logoff));
	aco_option_register(&cfg_info, "ackcall", ACO_EXACT, agent_types, "no", OPT_BOOL_T, 1, FLDSET(struct agent_cfg, ack_call));
	aco_option_register(&cfg_info, "acceptdtmf", ACO_EXACT, agent_types, "#", OPT_STRINGFIELD_T, 0, STRFLDSET(struct agent_cfg, dtmf_accept));
	aco_option_register(&cfg_info, "endcall", ACO_EXACT, agent_types, "yes", OPT_BOOL_T, 1, FLDSET(struct agent_cfg, end_call));
	aco_option_register(&cfg_info, "enddtmf", ACO_EXACT, agent_types, "*", OPT_STRINGFIELD_T, 0, STRFLDSET(struct agent_cfg, dtmf_end));
	aco_option_register(&cfg_info, "wrapuptime", ACO_EXACT, agent_types, "0", OPT_UINT_T, 0, FLDSET(struct agent_cfg, wrapup_time));
	aco_option_register(&cfg_info, "musiconhold", ACO_EXACT, agent_types, "default", OPT_STRINGFIELD_T, 0, STRFLDSET(struct agent_cfg, moh));
	aco_option_register_custom(&cfg_info, "group", ACO_EXACT, agent_types, "", agent_group_handler, 0);
	aco_option_register(&cfg_info, "recordagentcalls", ACO_EXACT, agent_types, "no", OPT_BOOL_T, 1, FLDSET(struct agent_cfg, record_agent_calls));
	aco_option_register(&cfg_info, "recordformat", ACO_EXACT, agent_types, "wav", OPT_STRINGFIELD_T, 0, STRFLDSET(struct agent_cfg, record_format));
	aco_option_register_custom(&cfg_info, "savecallsin", ACO_EXACT, agent_types, "", agent_savecallsin_handler, 0);
	aco_option_register_custom(&cfg_info, "custom_beep", ACO_EXACT, agent_types, "beep", agent_custom_beep_handler, 0);
	aco_option_register(&cfg_info, "password", ACO_EXACT, agent_types, "", OPT_STRINGFIELD_T, 0, STRFLDSET(struct agent_cfg, password));
	aco_option_register(&cfg_info, "fullname", ACO_EXACT, agent_types, "", OPT_STRINGFIELD_T, 0, STRFLDSET(struct agent_cfg, full_name));

	/*! \todo BUGBUG load_config() needs users.conf handling. */

	if (aco_process_config(&cfg_info, 0) == ACO_PROCESS_ERROR) {
		goto error;
	}

	return 0;

error:
	destroy_config();
	return -1;
}

/*! Agent config option override flags. */
enum agent_override_flags {
	AGENT_FLAG_ACK_CALL = (1 << 0),
	AGENT_FLAG_END_CALL = (1 << 1),
	AGENT_FLAG_DTMF_ACCEPT = (1 << 2),
	AGENT_FLAG_DTMF_END = (1 << 3),
	AGENT_FLAG_AUTO_LOGOFF = (1 << 4),
	AGENT_FLAG_WRAPUP_TIME = (1 << 5),
	AGENT_FLAG_MAX_LOGIN_TRIES = (1 << 6),
};

/*! \brief Structure representing an agent. */
struct agent_pvt {
	AST_DECLARE_STRING_FIELDS(
		/*! Identification of the agent.  (agents container key) */
		AST_STRING_FIELD(username);
		/*! Login override DTMF string for an agent to accept a call. */
		AST_STRING_FIELD(override_dtmf_accept);
		/*! Login override DTMF string for an agent to end a call. */
		AST_STRING_FIELD(override_dtmf_end);
	);
	/*! Flags show if settings were overridden by channel vars. */
	unsigned int flags;
	/*! Login override number of failed login attempts allowed. */
	unsigned int override_max_login_tries;
	/*! Login override number of seconds for agent to ack a call before being logged off. */
	unsigned int override_auto_logoff;
	/*! Login override time after a call in ms before the agent can get a new call. */
	unsigned int override_wrapup_time;
	/*! Login override if agent needs to ack a call to accept it. */
	int override_ack_call:1;
	/*! Login override if agent can use DTMF to end a call. */
	int override_end_call:1;

	/*! Mark and sweep config update to determine if an agent is dead. */
	unsigned int the_mark:1;
	/*!
	 * \brief TRUE if the agent is waiting to die.
	 *
	 * \note Agents cannot log in if they are dead.
	 *
	 * \note Agents destroy themselves when they are in the agent
	 * holding bridge.
	 */
	unsigned int dead:1;
	/*! TRUE if we joined the logged in channel to the bridging system. */
	unsigned int we_joined:1;

	/*! Custom device state of agent. */
	enum ast_device_state state;

	/*! When agent first logged in */
	time_t start_login;
	/*! When call started */
	time_t start_call;
	/*! When last disconnected */
	struct timeval last_disconnect;

	/*! Agent is logged in with this channel. (Holds ref) (NULL if not logged in.) */
	struct ast_channel *chan;
	/*! Active config values from config file. (Holds ref) */
	struct agent_cfg *cfg;
};

static void agent_pvt_destructor(void *vdoomed)
{
	struct agent_pvt *doomed = vdoomed;

	if (doomed->chan) {
		doomed->chan = ast_channel_unref(doomed->chan);
	}
	ao2_cleanup(doomed->cfg);
	doomed->cfg = NULL;
	ast_string_field_free_memory(doomed);
}

static struct agent_pvt *agent_pvt_new(struct agent_cfg *cfg)
{
	struct agent_pvt *agent;

	agent = ao2_alloc(sizeof(*agent), agent_pvt_destructor);
	if (!agent || ast_string_field_init(agent, 32)) {
		return NULL;
	}
	ast_string_field_set(agent, username, cfg->username);
	ao2_ref(cfg, +1);
	agent->cfg = cfg;
	agent->state = AST_DEVICE_UNAVAILABLE;
	return agent;
}

/*! Container of defined agents. */
static struct ao2_container *agents;

/*!
 * \internal
 * \brief Agents ao2 container sort function.
 * \since 12.0.0
 *
 * \param obj_left pointer to the (user-defined part) of an object.
 * \param obj_right pointer to the (user-defined part) of an object.
 * \param flags flags from ao2_callback()
 *   OBJ_POINTER - if set, 'obj_right', is an object.
 *   OBJ_KEY - if set, 'obj_right', is a search key item that is not an object.
 *   OBJ_PARTIAL_KEY - if set, 'obj_right', is a partial search key item that is not an object.
 *
 * \retval <0 if obj_left < obj_right
 * \retval =0 if obj_left == obj_right
 * \retval >0 if obj_left > obj_right
 */
static int agent_pvt_sort_cmp(const void *obj_left, const void *obj_right, int flags)
{
	const struct agent_pvt *agent_left = obj_left;
	const struct agent_pvt *agent_right = obj_right;
	const char *right_key = obj_right;
	int cmp;

	switch (flags & (OBJ_POINTER | OBJ_KEY | OBJ_PARTIAL_KEY)) {
	default:
	case OBJ_POINTER:
		right_key = agent_right->username;
		/* Fall through */
	case OBJ_KEY:
		cmp = strcmp(agent_left->username, right_key);
		break;
	case OBJ_PARTIAL_KEY:
		cmp = strncmp(agent_left->username, right_key, strlen(right_key));
		break;
	}
	return cmp;
}

/*!
 * \internal
 * \brief ao2_find() callback function.
 * \since 12.0.0
 *
 * Usage:
 * found = ao2_find(agents, agent, OBJ_POINTER);
 * found = ao2_find(agents, "agent-id", OBJ_KEY);
 * found = ao2_find(agents, agent->chan, 0);
 */
static int agent_pvt_cmp(void *obj, void *arg, int flags)
{
	const struct agent_pvt *agent = obj;
	int cmp;

	switch (flags & (OBJ_POINTER | OBJ_KEY | OBJ_PARTIAL_KEY)) {
	case OBJ_POINTER:
	case OBJ_KEY:
	case OBJ_PARTIAL_KEY:
		cmp = CMP_MATCH;
		break;
	default:
		if (agent->chan == arg) {
			cmp = CMP_MATCH;
		} else {
			cmp = 0;
		}
		break;
	}
	return cmp;
}

/*!
 * \internal
 * \brief Get the agent device state.
 * \since 12.0.0
 *
 * \param agent_id Username of the agent.
 *
 * \details
 * Search the agents container for the agent and return the
 * current state.
 *
 * \return Device state of the agent.
 */
static enum ast_device_state agent_pvt_devstate_get(const char *agent_id)
{
	RAII_VAR(struct agent_pvt *, agent, ao2_find(agents, agent_id, OBJ_KEY), ao2_cleanup);

	if (agent) {
		return agent->state;
	}
	return AST_DEVICE_INVALID;
}

static int agent_mark(void *obj, void *arg, int flags)
{
	struct agent_pvt *agent = obj;

	ao2_lock(agent);
	agent->the_mark = 1;
	ao2_unlock(agent);
	return 0;
}

static void agents_mark(void)
{
	ao2_callback(agents, 0, agent_mark, NULL);
}

static int agent_sweep(void *obj, void *arg, int flags)
{
	struct agent_pvt *agent = obj;
	int cmp = 0;

	ao2_lock(agent);
	if (agent->the_mark) {
		agent->the_mark = 0;
		agent->dead = 1;
		if (!agent->chan) {
			/* Agent isn't logged in at this time.  Destroy it now. */
			cmp = CMP_MATCH;
		}
	} else {
		/* Resurect a dead agent if it hasn't left yet or is still on a call. */
		agent->dead = 0;
	}
	ao2_unlock(agent);
	return cmp;
}

static void agents_sweep(void)
{
	ao2_callback(agents, OBJ_MULTIPLE | OBJ_UNLINK | OBJ_NODATA, agent_sweep, NULL);
}

static void agents_post_apply_config(void)
{
	struct ao2_iterator iter;
	struct agent_cfg *cfg;
	RAII_VAR(struct agents_cfg *, cfgs, ao2_global_obj_ref(cfg_handle), ao2_cleanup);

	ast_assert(cfgs != NULL);

	agents_mark();
	iter = ao2_iterator_init(cfgs->agents, 0);
	for (; (cfg = ao2_iterator_next(&iter)); ao2_ref(cfg, -1)) {
		RAII_VAR(struct agent_pvt *, agent, ao2_find(agents, cfg->username, OBJ_KEY), ao2_cleanup);

		if (agent) {
			ao2_lock(agent);
			agent->the_mark = 0;
			ao2_unlock(agent);
			continue;
		}
		agent = agent_pvt_new(cfg);
		if (!agent) {
			continue;
		}
		ao2_link(agents, agent);
	}
	ao2_iterator_destroy(&iter);
	agents_sweep();
}

static int unload_module(void)
{
	ast_devstate_prov_del("Agent");
	destroy_config();
	ao2_ref(agents, -1);
	agents = NULL;
	return 0;
}

static int load_module(void)
{
	int res = 0;

	agents = ao2_container_alloc_rbtree(AO2_ALLOC_OPT_LOCK_MUTEX,
		AO2_CONTAINER_ALLOC_OPT_DUPS_REPLACE, agent_pvt_sort_cmp, agent_pvt_cmp);
	if (!agents) {
		return AST_MODULE_LOAD_FAILURE;
	}
	if (load_config()) {
		ast_log(LOG_ERROR, "Unable to load config. Not loading module.\n");
		ao2_ref(agents, -1);
		agents = NULL;
		return AST_MODULE_LOAD_DECLINE;
	}

	/* Setup to provide Agent:agent-id device state. */
	res |= ast_devstate_prov_add("Agent", agent_pvt_devstate_get);

	if (res) {
		unload_module();
		return AST_MODULE_LOAD_FAILURE;
	}
	return AST_MODULE_LOAD_SUCCESS;
}

static int reload(void)
{
	if (aco_process_config(&cfg_info, 1) == ACO_PROCESS_ERROR) {
		/* Just keep the config we already have in place. */
		return -1;
	}
	return 0;
}

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_LOAD_ORDER, "Call center agent pool applications",
	.load = load_module,
	.unload = unload_module,
	.reload = reload,
	.load_pri = AST_MODPRI_DEVSTATE_PROVIDER,
);

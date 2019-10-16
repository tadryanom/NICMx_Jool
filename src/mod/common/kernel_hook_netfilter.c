#include "mod/common/kernel_hook.h"

#include "mod/common/log.h"
#include "mod/common/core.h"

static verdict find_instance(struct sk_buff *skb, struct xlator *result)
{
	int error;

	error = xlator_find_netfilter(dev_net(skb->dev), result);
	switch (error) {
	case 0:
		return VERDICT_CONTINUE;
	case -ESRCH:
		/*
		 * The hook functions are called on every packet whenever they
		 * reach a namespace, so not finding an instance here is
		 * perfectly normal and does not warrant an error message.
		 */
		return VERDICT_UNTRANSLATABLE;
	case -EINVAL:
		WARN(true, "xlator_find() is not supposed to EINVAL when iname is NULL.");
		return VERDICT_UNTRANSLATABLE;
	}

	WARN(true, "Unknown error code %d while trying to find iptables a Jool instance.",
			error);
	return VERDICT_UNTRANSLATABLE;
}

static unsigned int verdict2netfilter(verdict result, bool silence)
{
	switch (result) {
	case VERDICT_STOLEN:
		return NF_STOLEN; /* This is the happy path. */
	case VERDICT_UNTRANSLATABLE:
		if (!silence)
			log_debug("Returning the packet to the kernel.");
		return NF_ACCEPT;
	case VERDICT_DROP:
		log_debug("Dropping packet.");
		return NF_DROP;
	case VERDICT_CONTINUE:
		WARN(true, "At time of writing, Jool core is not supposed to return CONTINUE after the packet is handled.\n"
				"Please report this to the Jool devs.");
		return NF_ACCEPT; /* Hmmm... */
	}

	WARN(true, "Unknown verdict: %d", result);
	return NF_DROP;
}

/**
 * This is the function that the kernel calls whenever a packet reaches Jool's
 * IPv6 Netfilter hook.
 */
NF_CALLBACK(hook_ipv6, skb)
{
	struct xlation *state;
	verdict result;
	bool silence = true;

	state = xlation_create(NULL);
	if (!state)
		return NF_DROP;

	result = find_instance(skb, &state->jool);
	if (result != VERDICT_CONTINUE)
		goto end;
	silence = false;

	result = core_6to4(skb, state);

	xlator_put(&state->jool);
end:	xlation_destroy(state);
	return verdict2netfilter(result, silence);
}
EXPORT_SYMBOL_GPL(hook_ipv6);

/**
 * This is the function that the kernel calls whenever a packet reaches Jool's
 * IPv4 Netfilter hook.
 */
NF_CALLBACK(hook_ipv4, skb)
{
	struct xlation *state;
	verdict result;
	bool silence = true;

	state = xlation_create(NULL);
	if (!state)
		return NF_DROP;

	result = find_instance(skb, &state->jool);
	if (result != VERDICT_CONTINUE)
		goto end;
	silence = false;

	result = core_4to6(skb, state);

	xlator_put(&state->jool);
end:	xlation_destroy(state);
	return verdict2netfilter(result, silence);
}
EXPORT_SYMBOL_GPL(hook_ipv4);

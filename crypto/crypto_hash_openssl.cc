#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <common/factory.h>

#include <crypto/crypto_hash.h>

namespace {
	class InstanceEVP : public CryptoHash::Instance {
		LogHandle log_;
		const EVP_MD *algorithm_;
	public:
		InstanceEVP(const EVP_MD *algorithm)
		: log_("/crypto/hash/instance/openssl"),
		  algorithm_(algorithm)
		{ }

		~InstanceEVP()
		{ }

		Action *submit(Buffer *in, EventCallback *cb)
		{
			/*
			 * We process a single, large, linear byte buffer here rather
			 * than going a BufferSegment at a time, even though the byte
			 * buffer is less efficient than some alternatives, because
			 * there are padding and buffering implications if each
			 * BufferSegment's length is not modular to the block size.
			 */
			uint8_t indata[in->length()];
			in->moveout(indata, sizeof indata);

			uint8_t macdata[EVP_MD_size(algorithm_)];
			unsigned maclen;
			if (!EVP_Digest(indata, sizeof indata, macdata, &maclen, algorithm_, NULL)) {
				cb->param(Event::Error);
				return (cb->schedule());
			}
			ASSERT(log_, maclen == sizeof macdata);
			cb->param(Event(Event::Done, Buffer(macdata, maclen)));
			return (cb->schedule());
		}
	};

	class MethodOpenSSL : public CryptoHash::Method {
		LogHandle log_;
		FactoryMap<CryptoHash::Algorithm, CryptoHash::Instance> algorithm_map_;
	public:
		MethodOpenSSL(void)
		: CryptoHash::Method("OpenSSL"),
		  log_("/crypto/hash/openssl"),
		  algorithm_map_()
		{
			OpenSSL_add_all_algorithms();

			factory<InstanceEVP> evp_factory;
			algorithm_map_.enter(CryptoHash::MD5, evp_factory(EVP_md5()));
			algorithm_map_.enter(CryptoHash::SHA1, evp_factory(EVP_sha1()));
			algorithm_map_.enter(CryptoHash::SHA256, evp_factory(EVP_sha256()));

			/* XXX Register.  */
		}

		~MethodOpenSSL()
		{
			/* XXX Unregister.  */
		}

		std::set<CryptoHash::Algorithm> algorithms(void) const
		{
			return (algorithm_map_.keys());
		}

		CryptoHash::Instance *instance(CryptoHash::Algorithm algorithm) const
		{
			return (algorithm_map_.create(algorithm));
		}
	};

	static MethodOpenSSL crypto_hash_method_openssl;
}

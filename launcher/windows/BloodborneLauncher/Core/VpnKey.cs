using System.IO;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using Org.BouncyCastle.Asn1;
using Org.BouncyCastle.Asn1.Sec;
using Org.BouncyCastle.Asn1.X509;
using Org.BouncyCastle.Crypto;
using Org.BouncyCastle.Crypto.Generators;
using Org.BouncyCastle.Crypto.Operators;
using Org.BouncyCastle.Crypto.Parameters;
using Org.BouncyCastle.OpenSsl;
using Org.BouncyCastle.Pkcs;
using Org.BouncyCastle.Security;

namespace BloodborneLauncher.Core;

/// <summary>
/// The install's EC P-256 key: a new key with its certificate request (CN=nwvpn) at enrollment, and the signature that proves
/// the old key at renewal. On Windows this is CNG (.NET's ECDsa). Wine's CNG cannot make or load a P-256 key
/// (ECDsa.Create(nistP256) throws 0x80090029 under Proton-GE 11), so under Wine the same two operations are done with
/// BouncyCastle, managed code that needs no operating-system crypto. Both give the same formats: a SEC1 "EC PRIVATE KEY" PEM, a
/// PEM "CERTIFICATE REQUEST", and a DER ECDSA-SHA256 signature.
/// </summary>
public static class VpnKey
{
    /// <summary>BB_VPN_KEYS=managed / cng forces one (tests run both on Windows; the managed one is the Wine path).</summary>
    public static bool Managed => Environment.GetEnvironmentVariable("BB_VPN_KEYS") switch
    {
        "managed" => true,
        "cng" => false,
        _ => WineHost.IsWine,
    };

    public static (string KeyPem, string CsrPem) New() => Managed ? NewManaged() : NewCng();

    /// <summary>DER (RFC 3279 sequence) ECDSA-SHA256 signature of <paramref name="data"/> with the key.</summary>
    public static byte[] Sign(string keyPem, byte[] data) => Managed ? SignManaged(keyPem, data) : SignCng(keyPem, data);

    private static (string, string) NewCng()
    {
        using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
        return (key.ExportECPrivateKeyPem(), new CertificateRequest("CN=nwvpn", key, HashAlgorithmName.SHA256).CreateSigningRequestPem());
    }

    private static byte[] SignCng(string keyPem, byte[] data)
    {
        using var key = ECDsa.Create();
        key.ImportFromPem(keyPem);
        return key.SignData(data, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence);
    }

    private static (string, string) NewManaged()
    {
        var curve = SecNamedCurves.GetByName("secp256r1");
        var generator = new ECKeyPairGenerator();
        // the named-curve form (prime256v1), as ECDsa and OpenSSL write it: plain ECDomainParameters would put the whole curve in the key
        generator.Init(new ECKeyGenerationParameters(new ECNamedDomainParameters(SecObjectIdentifiers.SecP256r1, curve), new SecureRandom()));
        var pair = generator.GenerateKeyPair();
        var request = new Pkcs10CertificationRequest(new Asn1SignatureFactory("SHA256withECDSA", pair.Private),
            new X509Name("CN=nwvpn"), pair.Public, new DerSet());  // the [0] attributes field is mandatory, even empty
        return (Pem(pair.Private), Pem(request));
    }

    private static byte[] SignManaged(string keyPem, byte[] data)
    {
        using var reader = new StringReader(keyPem);
        if (new PemReader(reader).ReadObject() is not AsymmetricCipherKeyPair { Private: ECPrivateKeyParameters key })
            throw new CryptographicException("not an EC private key");
        var signer = SignerUtilities.GetSigner("SHA256withECDSA");
        signer.Init(true, key);
        signer.BlockUpdate(data, 0, data.Length);
        return signer.GenerateSignature();
    }

    private static string Pem(object value)
    {
        var text = new StringWriter { NewLine = "\n" };
        var writer = new PemWriter(text);
        writer.WriteObject(value);
        writer.Writer.Flush();
        return text.ToString();
    }
}

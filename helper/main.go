// pdg-helper: content-crypto helper for Proton Drive GUI.
//
// It uses Proton's own GopenPGP / go-proton-api, which understand Proton Drive's
// content format (including the transitional v6-PKESK + AES-GCM SEIPDv2 layout
// that librnp and strict RFC 9580 parsers reject). Two subcommands:
//
//	pdg-helper decrypt <unprotected-key-file>   < ciphertext > plaintext
//	pdg-helper upload   <job-json-file>         (prints the new link ID)
package main

import (
	"context"
	"crypto/sha1"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"mime"
	"os"
	"path/filepath"
	"time"

	"github.com/ProtonMail/go-proton-api"
	"github.com/ProtonMail/gopenpgp/v2/crypto"
	"github.com/ProtonMail/gopenpgp/v2/helper"
	"github.com/go-resty/resty/v2"
)

const uploadBlockSize = 4 * 1024 * 1024 // 4 MB

func die(err error) {
	os.Stderr.WriteString(err.Error() + "\n")
	os.Exit(1)
}

func main() {
	if len(os.Args) < 2 {
		os.Stderr.WriteString("usage: pdg-helper <decrypt|upload> ...\n")
		os.Exit(2)
	}
	switch os.Args[1] {
	case "decrypt":
		doDecrypt()
	case "upload":
		doUpload()
	default:
		os.Stderr.WriteString("unknown command: " + os.Args[1] + "\n")
		os.Exit(2)
	}
}

// ---- decrypt --------------------------------------------------------------
func doDecrypt() {
	if len(os.Args) < 3 {
		os.Stderr.WriteString("usage: pdg-helper decrypt keyfile < ciphertext\n")
		os.Exit(2)
	}
	armored, err := os.ReadFile(os.Args[2])
	if err != nil {
		die(err)
	}
	key, err := crypto.NewKeyFromArmored(string(armored))
	if err != nil {
		die(err)
	}
	kr, err := crypto.NewKeyRing(key)
	if err != nil {
		die(err)
	}
	cipher, err := io.ReadAll(os.Stdin)
	if err != nil {
		die(err)
	}
	plain, err := kr.Decrypt(crypto.NewPGPMessage(cipher), nil, 0)
	if err != nil {
		die(err)
	}
	os.Stdout.Write(plain.GetBinary())
}

// ---- upload ---------------------------------------------------------------
type uploadJob struct {
	UID              string `json:"uid"`
	AccessToken      string `json:"accessToken"`
	RefreshToken     string `json:"refreshToken"`
	AppVersion       string `json:"appVersion"`
	HostURL          string `json:"hostURL"`
	ShareID          string `json:"shareID"`
	AddressID        string `json:"addressID"`
	ParentLinkID     string `json:"parentLinkID"`
	SignatureAddress string `json:"signatureAddress"`
	AddrKey          string `json:"addrKey"`       // unprotected, armored
	ParentNodeKey    string `json:"parentNodeKey"` // unprotected, armored
	FilePath         string `json:"filePath"`
	FileName         string `json:"fileName"`
	ModTime          int64  `json:"modTime"` // unix seconds
}

// generateNodeKeys is ported from Proton-API-Bridge: a fresh x25519 node key
// protected by a random passphrase that is encrypted to the parent node keyring
// and signed by the address key.
func generateNodeKeys(kr, addrKR *crypto.KeyRing) (string, string, string, error) {
	token, err := crypto.RandomToken(32)
	if err != nil {
		return "", "", "", err
	}
	passphrase := base64.StdEncoding.EncodeToString(token)
	nodeKey, err := helper.GenerateKey("Drive key", "noreply@protonmail.com",
		[]byte(passphrase), "x25519", 0)
	if err != nil {
		return "", "", "", err
	}
	enc, err := kr.Encrypt(crypto.NewPlainMessageFromString(passphrase), nil)
	if err != nil {
		return "", "", "", err
	}
	encArm, err := enc.GetArmored()
	if err != nil {
		return "", "", "", err
	}
	sig, err := addrKR.SignDetached(crypto.NewPlainMessageFromString(passphrase))
	if err != nil {
		return "", "", "", err
	}
	sigArm, err := sig.GetArmored()
	if err != nil {
		return "", "", "", err
	}
	return nodeKey, encArm, sigArm, nil
}

func unlockNodeKR(parentNodeKR *crypto.KeyRing, nodeKey, passEnc string) (*crypto.KeyRing, error) {
	enc, err := crypto.NewPGPMessageFromArmored(passEnc)
	if err != nil {
		return nil, err
	}
	dec, err := parentNodeKR.Decrypt(enc, nil, crypto.GetUnixTime())
	if err != nil {
		return nil, err
	}
	locked, err := crypto.NewKeyFromArmored(nodeKey)
	if err != nil {
		return nil, err
	}
	unlocked, err := locked.Unlock(dec.GetBinary())
	if err != nil {
		return nil, err
	}
	return crypto.NewKeyRing(unlocked)
}

func doUpload() {
	if len(os.Args) < 3 {
		os.Stderr.WriteString("usage: pdg-helper upload jobfile\n")
		os.Exit(2)
	}
	raw, err := os.ReadFile(os.Args[2])
	if err != nil {
		die(err)
	}
	var job uploadJob
	if err := json.Unmarshal(raw, &job); err != nil {
		die(err)
	}
	ctx := context.Background()

	m := proton.New(proton.WithAppVersion(job.AppVersion), proton.WithHostURL(job.HostURL))
	c := m.NewClient(job.UID, job.AccessToken, job.RefreshToken)

	parentNodeKeyObj, err := crypto.NewKeyFromArmored(job.ParentNodeKey)
	if err != nil {
		die(err)
	}
	parentNodeKR, err := crypto.NewKeyRing(parentNodeKeyObj)
	if err != nil {
		die(err)
	}
	addrKeyObj, err := crypto.NewKeyFromArmored(job.AddrKey)
	if err != nil {
		die(err)
	}
	addrKR, err := crypto.NewKeyRing(addrKeyObj)
	if err != nil {
		die(err)
	}

	parentLink, err := c.GetLink(ctx, job.ShareID, job.ParentLinkID)
	if err != nil {
		die(err)
	}

	mimeType := mime.TypeByExtension(filepath.Ext(job.FileName))
	if mimeType == "" {
		mimeType = "application/octet-stream"
	}

	// Create draft: new node key + encrypted/signed passphrase.
	newNodeKey, newNodePassEnc, newNodePassSig, err := generateNodeKeys(parentNodeKR, addrKR)
	if err != nil {
		die(err)
	}
	req := proton.CreateFileReq{
		ParentLinkID:            parentLink.LinkID,
		MIMEType:                mimeType,
		NodeKey:                 newNodeKey,
		NodePassphrase:          newNodePassEnc,
		NodePassphraseSignature: newNodePassSig,
		SignatureAddress:        job.SignatureAddress,
	}
	if err := req.SetName(job.FileName, addrKR, parentNodeKR); err != nil {
		die(err)
	}
	// Parent hash key (decrypt without strict signature verification).
	encHK, err := crypto.NewPGPMessageFromArmored(parentLink.FolderProperties.NodeHashKey)
	if err != nil {
		die(err)
	}
	decHK, err := parentNodeKR.Decrypt(encHK, nil, 0)
	if err != nil {
		die(err)
	}
	if err := req.SetHash(job.FileName, decHK.GetBinary()); err != nil {
		die(err)
	}
	newNodeKR, err := unlockNodeKR(parentNodeKR, newNodeKey, newNodePassEnc)
	if err != nil {
		die(err)
	}
	sessionKey, err := req.SetContentKeyPacketAndSignature(newNodeKR)
	if err != nil {
		die(err)
	}

	res, err := c.CreateFile(ctx, job.ShareID, req)
	if err != nil {
		die(err)
	}
	linkID, revisionID := res.ID, res.RevisionID

	// Upload blocks.
	f, err := os.Open(job.FilePath)
	if err != nil {
		die(err)
	}
	defer f.Close()

	manifest := make([]byte, 0)
	blockSizes := make([]int64, 0)
	sha1d := sha1.New()
	totalSize := int64(0)
	for idx := 1; ; idx++ {
		buf := make([]byte, uploadBlockSize)
		n, rerr := io.ReadFull(f, buf)
		if n > 0 {
			buf = buf[:n]
			totalSize += int64(n)
			sha1d.Write(buf)
			blockSizes = append(blockSizes, int64(n))

			plain := crypto.NewPlainMessage(buf)
			encData, err := sessionKey.Encrypt(plain)
			if err != nil {
				die(err)
			}
			encSig, err := addrKR.SignDetachedEncrypted(plain, newNodeKR)
			if err != nil {
				die(err)
			}
			encSigArm, err := encSig.GetArmored()
			if err != nil {
				die(err)
			}
			h := sha256.Sum256(encData)
			manifest = append(manifest, h[:]...)

			blockReq := proton.BlockUploadReq{
				AddressID:  job.AddressID,
				ShareID:    job.ShareID,
				LinkID:     linkID,
				RevisionID: revisionID,
				BlockList: []proton.BlockUploadInfo{{
					Index:        idx,
					Size:         int64(len(encData)),
					EncSignature: encSigArm,
					Hash:         base64.StdEncoding.EncodeToString(h[:]),
				}},
			}
			links, err := c.RequestBlockUpload(ctx, blockReq)
			if err != nil {
				die(err)
			}
			if len(links) > 0 {
				if err := c.UploadBlock(ctx, links[0].BareURL, links[0].Token,
					resty.NewByteMultipartStream(encData)); err != nil {
					die(err)
				}
			}
		}
		if rerr == io.EOF || rerr == io.ErrUnexpectedEOF {
			break
		}
		if rerr != nil {
			die(rerr)
		}
	}

	// Commit the revision.
	manifestSig, err := addrKR.SignDetached(crypto.NewPlainMessage(manifest))
	if err != nil {
		die(err)
	}
	manifestSigArm, err := manifestSig.GetArmored()
	if err != nil {
		die(err)
	}
	commitReq := proton.CommitRevisionReq{
		ManifestSignature: manifestSigArm,
		SignatureAddress:  job.SignatureAddress,
	}
	xAttr := &proton.RevisionXAttrCommon{
		ModificationTime: time.Unix(job.ModTime, 0).Format("2006-01-02T15:04:05-0700"),
		Size:             totalSize,
		BlockSizes:       blockSizes,
		Digests:          map[string]string{"SHA1": hex.EncodeToString(sha1d.Sum(nil))},
	}
	if err := commitReq.SetEncXAttrString(addrKR, newNodeKR, xAttr); err != nil {
		die(err)
	}
	if err := c.CommitRevision(ctx, job.ShareID, linkID, revisionID, commitReq); err != nil {
		die(err)
	}
	os.Stdout.WriteString(linkID + "\n")
}

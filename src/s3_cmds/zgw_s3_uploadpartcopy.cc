#include "src/s3_cmds/zgw_s3_object.h"

#include "slash/include/env.h"
#include "src/zgwstore/zgw_define.h"
#include "src/s3_cmds/zgw_s3_xml.h"

bool UploadPartCopyCmd::DoInitial() {
  http_response_xml_.clear();

  if (!TryAuth()) {
    return false;
  }

  std::string source_path = req_headers_.at("x-amz-copy-source");
  SplitBySecondSlash(source_path, &src_bucket_name_, &src_object_name_);
  if (src_bucket_name_.empty() || src_object_name_.empty()) {
    http_ret_code_ = 400;
    GenerateErrorXml(kInvalidArgument, "x-amz-copy-source");
    return false;
  }

  upload_id_ = query_params_.at("uploadId");
  part_number_ = query_params_.at("partNumber");

  return true;
}

void UploadPartCopyCmd::DoAndResponse(pink::HttpResponse* resp) {
  if (http_ret_code_ == 200) {
    // Lock
    // Get src_object_ meta
    Status s = store_->GetObject(user_name_, src_bucket_name_, src_object_name_,
                                 &src_object_);
    if (!s.ok()) {
      if (s.ToString().find("Bucket Doesn't Belong To This User") !=
          std::string::npos) {
        http_ret_code_ = 404;
        GenerateErrorXml(kNoSuchBucket, bucket_name_);
      } else if (s.ToString().find("Object Not Found") != std::string::npos) {
        http_ret_code_ = 404;
        GenerateErrorXml(kNoSuchKey, object_name_);
      }
    } else {
      std::string virtual_bucket = "__TMPB" + upload_id_ +
        bucket_name_ + "|" + object_name_;

      // Initial new_object_
      new_object_.bucket_name = virtual_bucket;
      new_object_.object_name = part_number_;
      new_object_.etag = src_object_.etag;
      new_object_.size = src_object_.size;
      new_object_.owner = user_name_;
      new_object_.last_modified = slash::NowMicros();
      new_object_.storage_class = 0; // Unused
      new_object_.acl = "FULL_CONTROL";
      new_object_.upload_id = upload_id_;
      new_object_.data_block = src_object_.data_block;

      // Check uploadId
      zgwstore::Bucket dummy_bk;
      Status s = store_->GetBucket(user_name_, virtual_bucket, &dummy_bk);
      if (!s.ok()) {
        if (s.ToString().find("Bucket Doesn't Belong To This User") ||
            s.ToString().find("Bucket Not Found")) {
          http_ret_code_ = 404;
          GenerateErrorXml(kNoSuchUpload, upload_id_);
        } else {
          http_ret_code_ = 500;
        }
      } else {
        // TODO (gaodq) Add zp reference

        // Add part meta
        s = store_->AddObject(new_object_);
        if (!s.ok()) {
          http_ret_code_ = 500;
        }

        // Success
        GenerateRespXml();
      }
    }
    // Unlock
  }

  resp->SetStatusCode(http_ret_code_);
  resp->SetContentLength(http_response_xml_.size());
}

void UploadPartCopyCmd::GenerateRespXml() {
  assert(http_ret_code_ == 200);
  S3XmlDoc doc("CopyObjectResult");
  doc.AppendToRoot(doc.AllocateNode("LastModified",
                                    iso8601_time(new_object_.last_modified)));
  doc.AppendToRoot(doc.AllocateNode("ETag", new_object_.etag));

  doc.ToString(&http_response_xml_);
}

int UploadPartCopyCmd::DoResponseBody(char* buf, size_t max_size) {
  if (max_size < http_response_xml_.size()) {
    memcpy(buf, http_response_xml_.data(), max_size);
    http_response_xml_.assign(http_response_xml_.substr(max_size));
  } else {
    memcpy(buf, http_response_xml_.data(), http_response_xml_.size());
  }

  return std::min(max_size, http_response_xml_.size());
}

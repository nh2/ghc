{-# LANGUAGE Trustworthy #-}
{-# LANGUAGE NoImplicitPrelude #-}

module GHC.Event.Clock (getMonotonicTime, getMonotonicNSec) where

import GHC.Base
import GHC.Real
import Data.Word

-- | Return monotonic time in seconds, since some unspecified starting point
getMonotonicTime :: IO Double
getMonotonicTime = do w <- getMonotonicNSec
                      return (fromIntegral w / 1000000000)

-- | Return monotonic time in nanoseconds, since some unspecified starting point
foreign import ccall unsafe "getMonotonicNSec"
    getMonotonicNSec :: IO Word64

